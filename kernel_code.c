
//drivers/nvme/host/tcp.c

// block I/O request를 처리하는 함수. blk_status_t 값을 통해 I/O 요청의 성공, 실패를 확인함
static blk_status_t nvme_tcp_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	struct nvme_ns *ns = hctx->queue->queuedata; // hardware context 정보를 nvme namespace에 저장
	struct nvme_tcp_queue *queue = hctx->driver_data; // 이 hctx를 생성한 block driver에 포인터
	
	struct request *rq = bd->rq; // block I/O request를 추출
	struct nvme_tcp_request *req = blk_mq_rq_to_pdu(rq); // request의 1byte를 뛰어넘은 부분을 nvme_tcp_request로 가리키게함
	
	bool queue_ready = test_bit(NVME_TCP_Q_LIVE, &queue->flags); // NVMe_TCP_queue 상태 확인
	blk_status_t ret;

	if (!nvmf_check_ready(&queue->ctrl->ctrl, rq, queue_ready))
		return nvmf_fail_nonready_command(&queue->ctrl->ctrl, rq); // queue가 준비 안되어있을 때의 command 처리

	ret = nvme_tcp_setup_cmd_pdu(ns, rq); // request를 전송하기 위한 커맨드 PDU로 설정함
	if (unlikely(ret)) // 커맨드 PDU 설정에서 오류 있는지 확인
		return ret;

	blk_mq_start_request(rq); // request 처리 시작 상태 update
	nvme_tcp_queue_request(req, true, bd->last); // nvme_tcp_request를 tcp_queue에 넣기

	return BLK_STS_OK;
}

// block I/O request를 queue에 추가하고, request의 상태를 설정하여 I/O 처리 시작
// Block I/O 시작했음을 알리는 정보들을 update 해주는 느낌임
void blk_mq_start_request(struct request *rq)
{
	struct request_queue *q = rq->q;

	trace_block_rq_issue(q, rq); // request queue를 추적하기 위해 event 기록

	// queue의 flag정보 업데이트
	if (test_bit(QUEUE_FLAG_STATS, &q->queue_flags)) {
		rq->io_start_time_ns = ktime_get_ns();
		rq->stats_sectors = blk_rq_sectors(rq);
		rq->rq_flags |= RQF_STATS;
		rq_qos_issue(q, rq);
	}

	WARN_ON_ONCE(blk_mq_rq_state(rq) != MQ_RQ_IDLE);

	blk_add_timer(rq);
	WRITE_ONCE(rq->state, MQ_RQ_IN_FLIGHT);

#ifdef CONFIG_BLK_DEV_INTEGRITY
	if (blk_integrity_rq(rq) && req_op(rq) == REQ_OP_WRITE)
		q->integrity.profile->prepare_fn(rq);
#endif
}



//NVMe storage device로 block I/O request를 queue에 추가하고 처리함.
// bool sync : request를 동기적으로 처리할지 여부, 이게 뭔데???
// bool last : queue에 추가되는 request가 해당 queue의 마지막인지
static inline void nvme_tcp_queue_request(struct nvme_tcp_request *req,
		bool sync, bool last)
{
	struct nvme_tcp_queue *queue = req->queue; // nvme_tcp_request 구조체 안에 nvme_tcp_queue가 있음 -> 해당 request가 어느 queue 소속인지
	bool empty;

	empty = llist_add(&req->lentry, &queue->req_list) && // request를 대기중인 lockless request queue에 추가함
		list_empty(&queue->send_list) && !queue->request; // send 대기 중인 request가 없으며, 현재 처리중인 request도 없으면 empty true

	//근데 이렇게 request_queue에 req_list, send_list 두가지 종류의 list가 존재하는데 각각 어느 시점에서 쓰이는지는 파악 못하겠음

	/*
	 * if we're the first on the send_list and we can try to send
	 * directly, otherwise queue io_work. Also, only do that if we
	 * are on the same cpu, so we don't introduce contention.
	 */

	//현재 실행 중인 CPU가 I/O를 처리하는 CPU와 같고, 동기 처리가 활성화되었으며, 보내기 대기중인 요청도 없고, mutex lock이 가능할때
	if (queue->io_cpu == raw_smp_processor_id() &&
	    sync && empty && mutex_trylock(&queue->send_mutex)) {
		queue->more_requests = !last;
		nvme_tcp_send_all(queue); // 대기중인 모든 request 보냄
		queue->more_requests = false;
		mutex_unlock(&queue->send_mutex); // mutex 잠금 해제
	}
	// 마지막 request가 완료되었고, 추가 처리해야할 요청이 더 있는 경우
	if (last && nvme_tcp_queue_more(queue))
		queue_work_on(queue->io_cpu, nvme_tcp_wq, &queue->io_work);
}

/**
 * llist_add - add a new entry
 * @new:	new entry to be added
 * @head:	the head for your lock-less list
 *
 * Returns true if the list was empty prior to adding this entry.
 */
// Lockless 연결리스트는 동기화 없이 안전하게 요청을 추가하거나 제거할 수 있도록 설계되었습니다.
// 현재 lockless 연결리스트에 node 추가
static inline bool llist_add(struct llist_node *new, struct llist_head *head)
{
	return llist_add_batch(new, new, head); //이 함수는 주어진 연결리스트의 헤드에 새로운 노드들을 추가합니다.
}
// llist_add_batch 함수는 Lockless 연결리스트에 여러 개의 노드를 한 번에 추가하기 위한 함수입니다. 



// 송신 대기 중인 data를 가능한 모두 전송하는 함수
static inline void nvme_tcp_send_all(struct nvme_tcp_queue *queue)
{
	int ret;

	/* drain the send queue as much as we can... */
	do {
		ret = nvme_tcp_try_send(queue);
	} while (ret > 0);
	// 뭐라도 send 했으면 다시 send 시도함
}

// 송신 상태에 따라 request data를 순차적으로 전송하고, 송신 상태를 감시함.
static int nvme_tcp_try_send(struct nvme_tcp_queue *queue)
{
	struct nvme_tcp_request *req;
	int ret = 1;

	// 현재 송신 중인 요청이 없는 경우, 큐에서 새로운 요청을 가져옴
	if (!queue->request) {
		queue->request = nvme_tcp_fetch_request(queue);
		if (!queue->request)
			return 0;
	}
	req = queue->request; // 변수 기니까 짧은 변수명에 tcp_request 포인터 옮김

	// request state에 따라서 CMP_PDU, H2C_PDU, DATA, DDGST를 송신함
	// 근데 else if가 아니라 if로 send를 하고 나서도 확인하네? -> 단계적으로 send를 하는 과정인가?
	// ret <= 0이면 done:으로 가서 문제 상황 해결하러 감
	if (req->state == NVME_TCP_SEND_CMD_PDU) {
		ret = nvme_tcp_try_send_cmd_pdu(req);
		if (ret <= 0)
			goto done;
		if (!nvme_tcp_has_inline_data(req))
			return ret;
	}

	if (req->state == NVME_TCP_SEND_H2C_PDU) {
		ret = nvme_tcp_try_send_data_pdu(req);
		if (ret <= 0)
			goto done;
	}

	if (req->state == NVME_TCP_SEND_DATA) {
		ret = nvme_tcp_try_send_data(req);
		if (ret <= 0)
			goto done;
	}

	if (req->state == NVME_TCP_SEND_DDGST)
		ret = nvme_tcp_try_send_ddgst(req);
//-EAGAIN : 일시적인 상황에서 작업이 완료되지 않았음을 의미함, 나중에 다시 시도하면 성공할 가능성이 있는 경우 반환된다.
done:
	if (ret == -EAGAIN) {
		ret = 0;
	} // 나중에 다시 시도하면 될 수도 있으니까 ret=0으로 tcp_send_all 멈춤 
	else if (ret < 0) { // 걍 송신 실패한거임, 오류 메시지 출력하고 해당 요청 실패 처리
		dev_err(queue->ctrl->ctrl.device,
			"failed to send request %d\n", ret);
		if (ret != -EPIPE && ret != -ECONNRESET)
			nvme_tcp_fail_request(queue->request);
		nvme_tcp_done_send_req(queue);
	}
	return ret;
}
// i10_queue_init, i10_queue_alloc 등등 이런 함수들로 먼저 queue를 세팅하고 나서, i10_host_queue_rq가 돌아가는 것 같음.



// core, driver 등등 정보 뽑아내고 request를 네트워크 전송을 위한 unit으로 바꿈
static blk_status_t i10_host_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
    //nvme_tcp에서 i10_host로 변경되었음
	struct nvme_ns *ns = hctx->queue->queuedata;
	struct i10_host_queue *queue = hctx->driver_data;
	struct request *rq = bd->rq;
	struct i10_host_request *req = blk_mq_rq_to_pdu(rq);
	bool queue_ready = test_bit(I10_HOST_Q_LIVE, &queue->flags);
	blk_status_t ret;

	if (!nvmf_check_ready(&queue->ctrl->ctrl, rq, queue_ready))
		return nvmf_fail_nonready_command(&queue->ctrl->ctrl, rq);

	ret = i10_host_setup_cmd_pdu(ns, rq); // request를 command PDU로 설정
	if (unlikely(ret))
		return ret;

	blk_mq_start_request(rq); // 원래 커널 코드 - Block I/O 시작했음을 알리기 위한 state들 update하는

	i10_host_queue_request(req); // i10 queue에 request 던짐

	return BLK_STS_OK;
}


static inline void i10_host_queue_request(struct i10_host_request *req)
{
	struct i10_host_queue *queue = req->queue;

    // queue를 잠궈놓고, request를 queue의 send_list tail에 추가함
    // 기존 nvme_tcp_queue_request에서는 lockless리스트인 req_list에 넣었는데
    // i10은 그냥 바로 전송 중인 요청들을 담아두는 send_list에 넣네?
	spin_lock(&queue->lock);
	list_add_tail(&req->entry, &queue->send_list);
	spin_unlock(&queue->lock);

    //이 if문 안에 함수 두개 모두 false가 return이 안되면, doorbell 바로 울리러 else문으로 감
	if (!i10_host_legacy_path(req) && !i10_host_is_nodelay_path(req)) 
    {
		queue->nr_req++;

		/* Start a new delayed doorbell timer */
		// i10 queue가 비어있는 상태였고, 처음 request가 들어오면, timer 설정
        if (!hrtimer_active(&queue->doorbell_timer) &&
			queue->nr_req == 1)
			hrtimer_start(&queue->doorbell_timer,
				ns_to_ktime(i10_delayed_doorbell_us *
					NSEC_PER_USEC),
				HRTIMER_MODE_REL);
		/* Ring the delayed doorbell
		 * if I/O request counter >= i10 aggregation size
		 */
		else if (queue->nr_req >= I10_AGGREGATION_SIZE) 
        {
			if (hrtimer_active(&queue->doorbell_timer))
				hrtimer_cancel(&queue->doorbell_timer);
			queue_work_on(queue->io_cpu, i10_host_wq,
					&queue->io_work);
		}
	}
	/* Ring the doorbell immediately for no-delay path */
	else 
    {
		if (hrtimer_active(&queue->doorbell_timer))
			hrtimer_cancel(&queue->doorbell_timer);
		queue_work_on(queue->io_cpu, i10_host_wq, &queue->io_work);
	}
}

// legacy_path인지 확인하는 함수
// 두가지 조건을 다 만족을 안해야 legacy path임
static inline bool i10_host_legacy_path(struct i10_host_request *req)
{
	return (i10_host_queue_id(req->queue) == 0) || // queue의 index가 0이면 안된다?
		(i10_delayed_doorbell_us < I10_MIN_DOORBELL_TIMEOUT); // 정확히 이게 뭔지 모르겠네?
        //I10_MIN_DOOBELL_TIMEOUT이 뭐야
}

// i10_host_queue들을 가리키는 포인터 배열 주소를 빼버리네? -> queue의 index가 나오지
static inline int i10_host_queue_id(struct i10_host_queue *queue)
{
	return queue - queue->ctrl->queues;
}

//request가 nodelay_path인지 확인함
static bool i10_host_is_nodelay_path(struct i10_host_request *req)
{
	return (req->curr_bio == NULL) || // 현재 request의 curr_bio 필드가 NULL인지 확인(현재 처리 중인 BIO가 있는지?)
		(req->curr_bio->bi_opf & ~I10_ALLOWED_FLAGS); // 만약 현재 처리 중인 BIO의 bi_opf 필드와 I10_ALLOWED_FLAGS 비트와의 논리 AND 결과가 0이 아닌 경우, 이 역시 "no-delay path"로 간주됩니다. bi_opf는 BIO의 작업 플래그를 나타내는데, 여기서 I10_ALLOWED_FLAGS는 특정 플래그들을 나타냅니다. 비트 AND 결과가 0이 아닌 경우, 이는 허용되지 않는 작업 플래그가 포함되었다는 것을 의미하며, 이 경우에도 "no-delay path"로 간주됩니다???
}


// 특정 cpu에 사용할 작업 queue와 예약할 work를 넣어서 작업을 예약하는 것
bool queue_work_on(int cpu, struct workqueue_struct *wq,
		   struct work_struct *work)
{
	bool ret = false;
	unsigned long flags;

	local_irq_save(flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_work(cpu, wq, work);
		ret = true;
	}

	local_irq_restore(flags);
	return ret;
}


반환값: 만약 @work가 이미 큐에 있었다면 %false를, 그렇지 않으면 %true를 반환합니다.

이 함수는 지정된 작업 큐에 작업을 예약하는 역할을 합니다. 특정 CPU에 작업을 예약하려는 경우 사용하며, 이미 해당 작업이 큐에 예약되어 있는지 여부를 확인하여 중복 예약을 방지합니다. 이를 통해 다른 CPU에서 해당 작업을 동시에 예약하는 것을 방지하고, 큐에 예약된 작업이 중복되지 않도록 합니다. 함수 내부에서는 원자적인 연산을 사용하여 예약 여부를 확인하고, 필요한 경우 작업을 큐에 예약합니다.
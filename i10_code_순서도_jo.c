/*
1. i10_queue_init, i10_queue_alloc 등등 이런 함수들로 먼저 i10 queue를 세팅함 - hrtimer까지

i10_host_init_connection이라는 함수가 있는데, 이게 i10_host_alloc_queue 여기서 호출된다.
이건 또 i10_host_alloc_admin_queue 여기서 호출되는데, 이건 또 i10_host_configure_admin_queue 여기서 호출된다.
이건 또 i10_host_setup_ctrl 여기서 호출된다, 이건 또 i10_host_create_ctrl 여기서 호출되는데
또 이 i10_host_create_ctrl 함수는 최종적으로 struct nvmf_transport_ops 구조체 변수인 i10_host_transport에 함수 포인터로 저장이 된다.

-> 그리하여 static int __init i10_host_init_module(void) 이 함수에서 nvmf_register_transport(&i10_host_transport);
이렇게 호출당해서 여기에서부터 쭉 i10 전용 자원이 할당되는 것 같음
(이 자세한 기능들은 다 알아야 되는지 모르겠네??? 5.10.73에서 nvme_tcp_init_connection 이런식으로 i10_host대신 nvme_tcp로 바꿔서 찾은 다음 gpt한테 물어보면 대답해주긴 함)

-> 근데 hrtimer를 초기화하는 부분이 i10_host_alloc_queue에서 있는데, 이때 queue 세부 정보를 전부 초기화하는 것으로 보아 admin,setup_ctrl은 더 hardware와 밀접한 부분을 설정하는 함수같음.

-> nvme-oF 표준에 따라서 만들어지는 초기에 connect할때 다 세팅이 된다.
-> 즉, 굳이 현재 단계에서 볼 필요는 없음..


2. 이후 어디서 호출되는 것인지는 모르겠지만, i10_host_queue_rq가 호출되면서 block I/O request가 넘어옴

3. blk_mq 구조체에서 hardware정보, request 정보 뽑아내고

4. blk_mq에다가 request 처리 시작했다고 알려준 뒤에, i10_host_queue_request를 호출해서 request 던짐

5. i10_host_queue_request 일단 i10_host_queue의 send_list tail에다가 i10_host_request->entry 추가함

6. 두가지 함수를 통해서 nodelay_path인지 확인한 다음, 

6.1. nodelay_path가 아니라면 i10_queue의 timer 실행시키거나, aggregation size 확인

6.2. nodelay_path라면 바로 queue->io_cpu에 working 예약 걸어버림


=> 정상적으로 timeout이 발생하면 i10_host_doorbell_timeout이 호출되는데 여기서 aggregation size를 확인하고 이걸 동적으로 조절하는 방식을 시도해보면 좋을 것 같다는 생각.
(그래서 i10_host_doorbell_timeout에 대해서 공부하는 중)

*/




// Block layer에서 I/O request를 i10 layer로 넘겨주는 맨처음 함수같음
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


//기존 nvme_tcp에서는 초기에 request를 queue에 insert할때, req_list와 send_list 두가지로 request를 관리하는데
//i10은 send_list만 씀, 그 이유에 대해서는 파악해볼 필요가 있을 것 같음?
static inline void i10_host_queue_request(struct i10_host_request *req)
{
	struct i10_host_queue *queue = req->queue;

    // queue를 잠궈놓고, request를 queue의 send_list tail에 추가함
    // 기존 nvme_tcp_queue_request에서는 lockless리스트인 req_list에 넣었는데
    // i10은 그냥 바로 전송 중인 요청들을 담아두는 send_list에 넣네?
	spin_lock(&queue->lock); // queue를 잠근다는 것은 동시에 access할 수도 있다. 라는 것인데 어떤 구조로 동시에 queue에 access하는 것인지는 봐야 할 듯?
	list_add_tail(&req->entry, &queue->send_list);
	spin_unlock(&queue->lock);
	// 현재로서는 timer 돌아가서 doorbell timeout되고 queue_work_on이 실행되면 queue에 넣고 빼는게 충돌나서 lock 걸어주는 건가 싶음.

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

	local_irq_save(flags); // flags에 인터럽트 상태 저장, 인터럽트 비활성화

	// 이미 work가 queue에 있었다면 이거 건너뛰고 false가 return되게 하는거임
	// test bit를 통해, 해당 작업을 동시에 예약하는 것을 방지하는 원자적인 연산을 하는 것 같음. - 운체 조금 복습하면 확실히 알 듯?
	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_work(cpu, wq, work);
		ret = true;
	}

	local_irq_restore(flags); // flags를 통해 인터럽트 상태 복원
	//다중 cpu 시스템에서 동시성을 관리하거나, 임계 영역에서 인터럽트 경합을 방지하기 위해서 이렇게 인터럽트 저장&복원을 한다는데 이 정도로만 알아도 될 듯
	return ret;
}



//hrtimer 만료되면 자동으로 호출되는 함수 -> i10_host_alloc_queue에서 queue 만들면서 queue의 doorbell pointer로 함수 포인터 세팅해뒀음
//timer가 울린 queue를 cpu에 작업 예약 걸어주는 함수임.
enum hrtimer_restart i10_host_doorbell_timeout(struct hrtimer *timer)
{
	// timer라는 매개변수를 사용해서 만료된 timer가 속한 i10_host_queue를 찾는 것 같음???
	//container_of가 뭐하는 함수? 매크로?인지 찾아야 할 듯 > container_of 매크로는 포인터를 이용해서 해당 포인터가 속한 구조체의 포인터를 계산해줌
	struct i10_host_queue *queue =
		container_of(timer, struct i10_host_queue,
			doorbell_timer);
	//해당 queue 찾았으면, timeout이니까 cpu에 작업 예약 걸어야지
	queue_work_on(queue->io_cpu, i10_host_wq, &queue->io_work);
	return HRTIMER_NORESTART; //함수가 끝날 때 HRTIMER_NORESTART를 반환하여 HRTimer의 재시작이 필요하지 않음을 알려줍니다.
}


//어라 이건 그냥 timeout이네?, gpt한테 물어보고 공부해봐야 할 듯
//block device의 타임아웃을 처리하는 함수
//block device에서 IO request가 타임아웃 났을 때 호출됨
static enum blk_eh_timer_return i10_host_timeout(struct request *rq, bool reserved)
{
	struct i10_host_request *req = blk_mq_rq_to_pdu(rq);
	struct i10_host_ctrl *ctrl = req->queue->ctrl;
	struct nvme_tcp_cmd_pdu *pdu = req->pdu;

	dev_warn(ctrl->ctrl.device,
		"queue %d: timeout request %#x type %d\n",
		i10_host_queue_id(req->queue), rq->tag, pdu->hdr.type);

	if (ctrl->ctrl.state != NVME_CTRL_LIVE) {
		/*
		 * Teardown immediately if controller times out while starting
		 * or we are already started error recovery. all outstanding
		 * requests are completed on shutdown, so we return BLK_EH_DONE.
		 */
		flush_work(&ctrl->err_work);
		i10_host_teardown_io_queues(&ctrl->ctrl, false);
		i10_host_teardown_admin_queue(&ctrl->ctrl, false);
		return BLK_EH_DONE;
	}

	dev_warn(ctrl->ctrl.device, "starting error recovery\n");
	i10_host_error_recovery(&ctrl->ctrl);

	return BLK_EH_RESET_TIMER;
}

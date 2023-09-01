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


enum hrtimer_restart i10_host_doorbell_timeout(struct hrtimer *timer)
=> 여기에서 aggregation size를 조절하는게 목표인데 여기에서 드는 의문점.

1. 논문에서 말하는 lane은 nvme link를 생성하는 과정에서 nvme-oF에 따라서 생기는거라고 하셨는데,
fio돌려서 core 여러개로 늘리면 그에 맞춰서 lane이 늘어나는게 아니라 lane이 고정되어 있는건가??? -> 이제 논문에서의 core가 우리 실험 환경에서의 ens102f0에 대응되는건가?...


2. 1번 의문점이 들었던 이유가 이제 single core에서 테스트를 할 때에는 lane이 어차피 한개인 실험을 할테니까 상관이 없는데
lane이 여러개가 되면 이 lane마다 aggregation size를 따로 적용을 시켜야 될텐데 이러면 전역변수가 아닌, queue 구조체 멤버 변수로 넣어야 되는거 아닐까???
라는 생각에서 문득 1번 의문점이 떠오름.

request의 양이 lane마다 다를테니까?
근데 그걸 우리가 테스트하는 fio 환경에서는 어떻게 테스트 하는거지?
논문에서의 core가 우리가 테스트하는 fio 환경에서의 cpu_allowed 해주는 cpu랑 같나?...


위 궁금증에 대해서는 lane이 이미 nvme link 생성하면서 nvme cli이 core 갯수 파악해서 target이랑 맵핑 해줬으니까 lane은 이미 만들어져 있다고 생각하면 되고
lane 마다 aggregation size를 따로 가져야 될테니 어딘가의 멤버 변수로 가지는게 맞다!
그리고 IOPS의 양은 iodepth를 조절하면서 진행하는 것이 맞다.

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
	
	// queue_work_on 함수는 작업을 담당하는 커널 worker thread를 깨우는 역할
	queue_work_on(queue->io_cpu, i10_host_wq, &queue->io_work);
	//여기서 thread가 깨어나면 queue->io_work를 호출하는데
	//i10_host_alloc_queue부분에서 INIT_WORK(&queue->io_work, i10_host_io_work); 로 바인딩이 되어 있음
	//i10_host_io_work 부분을 확인해봐야함
	return HRTIMER_NORESTART; //함수가 끝날 때 HRTIMER_NORESTART를 반환하여 HRTimer의 재시작이 필요하지 않음을 알려줍니다.
}


//queue->io_work에 바인딩 되어 있는 함수
//i10 host queue에서 수행되야하는 IO request들을 처리하는 함수
static void i10_host_io_work(struct work_struct *w)
{
	//인자로 들어온 work_struct에서 큐를 가져옴
	struct i10_host_queue *queue =
		container_of(w, struct i10_host_queue, io_work);
	//현재 시간에서 1 msec를 더해서 start에 저장(IO request 처리에 대한 제한시간을 두기 위함)
	unsigned long start = jiffies + msecs_to_jiffies(1);

	//1msec 동안 IO request를 처리하는 시도를 하는 것 같음
	do {
		bool pending = false;
		int result;

		//queue에서 데이터를 보내는 시도를 함(result는 보낸 데이터 양이나 오류 코드를 저장함)
		result = i10_host_try_send(queue);
		
		//성공 : data를 보냈거나, caravan 형태로 묶었다??? <- req_state에 따른 ret 값을 분석해봐야 제대로 알 것 같음
		if (result > 0) {
			pending = true; //성공하면 pending = true
		} 
		//실패 <- 이건... 실패할 일이 없을거 같으니 분석을 안해도 될 거 같음, maybe?
		else if (unlikely(result < 0)) {
			dev_err(queue->ctrl->ctrl.device,
				"failed to send request %d\n", result);
			//-EPIPE(파이프가 깨진 상태라고 함)가 아닌 다른 오류면 해당 request를 실패처리하고 작업 완료
			if (result != -EPIPE)
				i10_host_fail_request(queue->request);
			i10_host_done_send_req(queue);
			return;
		}
		result = i10_host_try_recv(queue); //queue에서 데이터를 받아오는 시도를 함
		if (result > 0) //데이터 잘 받아오면 pending = true
			pending = true;
		//실패하면 함수 종료
		if (!pending)
			return;

	} while (time_before(jiffies, start));
	//1msec 내에 처리 못하면 다시 큐에 추가해서 나중에 처리하도록 함
	//우리 테스트에서는 이 일이 일어나지 않겠지 1msec동안 request 처리를 못하다니 성능 박살일듯
	queue_work_on(queue->io_cpu, i10_host_wq, &queue->io_work);
}


// i10 host queue에 있는 request를 보내는 함수 
static int i10_host_try_send(struct i10_host_queue *queue)
{
	struct i10_host_request *req;
	int ret = 1;

	// 현재 queue에 처리할 요청이 있는지 확인함
	if (!queue->request) {
		queue->request = i10_host_fetch_request(queue); //처리할 요청 없으면 queue에서 request 가져옴
		if (!queue->request && !queue->caravan_len) // queue에서 가져올 request도 없고 caravan도 없으면 try_send 종료
			return 0; // 보낼 data가 없다는 의미겠지
	}

	/* Send i10 caravans now */
	// 우리가 테스트하는 상황에서 doorbell이 울린 직후에는 이게 true일리 없음. 아직 caravan 생성도 안한 request 상태임
	if (i10_host_send_caravan(queue)) { // 이게 true면 caravan 당장 쏴야함
		
		// caravan data를 소켓으로 전송하는 과정인 것 같음
		struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_EOR };
		int i, i10_ret;

		if (i10_host_sndbuf_nospace(queue, queue->caravan_len)) {
			set_bit(SOCK_NOSPACE,
				&queue->sock->sk->sk_socket->flags);
			return 0;
		}

		i10_ret = kernel_sendmsg(queue->sock, &msg,
				queue->caravan_iovs,
				queue->nr_iovs,
				queue->caravan_len);

		if (unlikely(i10_ret <= 0)) {
			dev_err(queue->ctrl->ctrl.device,
				"I10_HOST: kernel_sendmsg fails (i10_ret %d)\n",
				i10_ret);
			return i10_ret;
		}

		for (i = 0; i < queue->nr_mapped; i++)
			kunmap(queue->caravan_mapped[i]);

		queue->nr_req = 0;
		queue->nr_iovs = 0;
		queue->nr_mapped = 0;
		queue->caravan_len = 0;
		queue->send_now = false;
	}

	if (queue->request) // 현재 queue에 처리할 요청이 있으면
		req = queue->request; // i10_host_request 구조체인 req에 현재 처리할 request 포인터 옮겨서 저장함
	else
		return 0;

	// request가 있으니 request 상태에 따라서 이게 호출이 되겠지
	// 이런거 호출을 하다보면 caravan 생성되고, send_now가 true가 되기도 함 - 일단 내용 존나 많아져서 여기 이후로는 분석 시도 안해봄
	if (req->state == I10_HOST_SEND_CMD_PDU) {
		ret = i10_host_try_send_cmd_pdu(req);
		if (ret <= 0)
			goto done;
		if (!i10_host_has_inline_data(req))
			return ret;
	}

	if (req->state == I10_HOST_SEND_H2C_PDU) {
		ret = i10_host_try_send_data_pdu(req);
		if (ret <= 0)
			goto done;
	}

	if (req->state == I10_HOST_SEND_DATA) {
		ret = i10_host_try_send_data(req);
		if (ret <= 0)
			goto done;
	}

	if (req->state == I10_HOST_SEND_DDGST)
		ret = i10_host_try_send_ddgst(req);
done:
	if (ret == -EAGAIN)
		ret = 0;
	return ret;
}


/* 
3가지 조건을 확인해서 queue에 있는 data를 전송할지 여부를 판단함
정확히 이해한건 아니지만 추측을 해보자면
일단 우리는 doorbell이 울린 상황을 생각하고 있는거니까, timeout에서 HRTIMER_NORESTART 이걸 반환했으니 timer는 꺼져있을거임
근데 doorbell이 울린 상황을 생각해보면 send_now는 당연히 false고, queue->request는 있는 상태일테니까
doorbell이 울린 직후의 i10_host_send_caravan을 false를 반환할 것임
*/
static bool i10_host_send_caravan(struct i10_host_queue *queue)
{
	/* 1. Caravan becomes full (64KB), : i10 caravan이 꽉 찬 경우
	 * 2. No-delay request arrives,  : no-delay request가 도착한 경우
	 * 3. No more request remains in i10 queue : queue 내부에 더 이상 처리할 request가 없는 경우
	 */
	return queue->send_now || // send_now는 caravan이 full이거나, nodelay_path일때 true로 설정된다
		(!hrtimer_active(&queue->doorbell_timer) && // timer가 꺼져있으면 true && 
		!queue->request && queue->caravan_len); // 현재 처리해야할 request가 없으면 true && caravan이 있으면 true
	// send_now가 true이거나
	// (timer 꺼져 있음, 현재 처리해야할 request가 없음, caravan이 있음) 세가지 조건을 만족하면 true 반환
}


// 소켓을 통해서 뭘 주고 받는데... 정체가 뭐냐 이거
// 내가 지금까지 보낸 data 잘 받았는지, 얼마나 받았는지 알려줘 이건거 같은데
static int i10_host_try_recv(struct i10_host_queue *queue)
{
	struct socket *sock = queue->sock;
	struct sock *sk = sock->sk;
	read_descriptor_t rd_desc;
	int consumed;

	rd_desc.arg.data = queue;
	rd_desc.count = 1;
	lock_sock(sk);
	consumed = sock->ops->read_sock(sk, &rd_desc, i10_host_recv_skb);
	release_sock(sk);
	return consumed;
}



//block device의 타임아웃을 처리하는 함수
//block device에서 IO request가 타임아웃 났을 때 호출됨
static enum blk_eh_timer_return i10_host_timeout(struct request *rq, bool reserved)
{
	//해당 IO request의 정보들을 가져옴
	struct i10_host_request *req = blk_mq_rq_to_pdu(rq);
	struct i10_host_ctrl *ctrl = req->queue->ctrl;
	struct nvme_tcp_cmd_pdu *pdu = req->pdu;

	//device 로그에 타임아웃된 request와 관련 정보 기록(큐 ID, request tag, pdu 유형)
	dev_warn(ctrl->ctrl.device,
		"queue %d: timeout request %#x type %d\n",
		i10_host_queue_id(req->queue), rq->tag, pdu->hdr.type);


	//컨트롤러(?)의 상태가 LIVE가 아닐 때 = 컨트롤러가 시작 중이거나 이미 에러 복구 중인 상황
	if (ctrl->ctrl.state != NVME_CTRL_LIVE) {
		/*
		 * Teardown immediately if controller times out while starting
		 * or we are already started error recovery. all outstanding
		 * requests are completed on shutdown, so we return BLK_EH_DONE.
		 */
		//컨트롤러의 에러 복구 작업 queue에서 대기 중인 모든 작업을 실행 보장해줌
		flush_work(&ctrl->err_work);
		//컨트롤러 IO queue, admin queue 정리 및 폐기
		i10_host_teardown_io_queues(&ctrl->ctrl, false);
		i10_host_teardown_admin_queue(&ctrl->ctrl, false);
		return BLK_EH_DONE;
	}
	//컨트롤러의 로그에 에러 복구가 시작됨을 기록
	dev_warn(ctrl->ctrl.device, "starting error recovery\n");
	//에러 복구 작업 시작
	i10_host_error_recovery(&ctrl->ctrl);
	//에러가 복구가 시작되었으므로, 에러 핸들러 타이머 재시작하도록 BLK_EH_RESET_TIMER 반환 
	return BLK_EH_RESET_TIMER;
}

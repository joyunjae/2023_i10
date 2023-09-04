/* iodepth == 4일때의 kernel log 분석
1. i10_target_data_ready이 호출되어 host로부터 socket을 받음 -> thread 깨움
2. thread에서 i10_target_io_work 실행
3. i10_target_try_recv가 호출되어서 budget만큼 request를 receive하려고 함
4-1. i10_target_try_recv_one이 호출되어서 recv state에 따라서 receive함 
4-2. recv->state == PDU 라서 i10_target_try_recv_pdu이게 호출된다.
4-3. i10_target_try_recv_pdu이게 i10_target_done_recv_pdu이걸 또 호출함
4-4. i10_target_done_recv_pdu에서는 i10_target_queue_response가 호출된다
4-5. i10_target_queue_response에서는 nvmet_req에서 i10_target_cmd를 뽑아내는데,
        또 queue_work_on을 실행시켜서 cmd의 queue의 io_work를 돌아가게 예약을 함. (처음보는 구조체 등장)
4-6. i10_target_try_recv_pdu의 return 값은 0이 나오면서 끝나고

5. 이렇게 i10_target_try_recv_one이 끝나서 이 결과의 ret값은 1이 나오게 된다.
5-1. 이게 1이니까 ops++하고 한번더 i10_target_try_recv의 budget수에 따른 try_recv_one이 호출된다.
5-2. 여기선 이제 더 이상 receive할게 없으니까 4-2~4-6 쭉 도는지 중간에 끊기는지는 모르겠다만 -11을 return함
5-3. 이후 i10_target_try_recv_one은 0을 return함

6. i10_target_try_recv_one이 0을 return하면서 i10_target_try_recv가 종료된다.
    (i10_target_try_recv_one을 budget내에서 다 돌렸으니 i10_target_try_recv도 0을 return함)

7. i10_target_try_recv가 0을 return하면서 pending이 false인채로 i10_target_try_send를 호출하러 감
 
8-1. i10_target_try_send에서도 budget내로 i10_target_try_send_one을 호출함
8-2. 여기서도 PDU라서 try_send_data_pdu를 호출함
8-3. i10_target_try_send_data_pdu의 return 값이 1이 되고,   

9-1. 그렇게 끝나는 줄 알았으나 cmd->state가 이번엔 send_data가 되어서 i10_target_try_send_data를 호출함
9-2. i10_target_try_send_data도 1을 return함

10. 최종적으로 i10_target_try_send_one은 1을 return함
11. budget 여유 있고, ret도 1이니까 i10 caravans send 피해서 for문 한번 더 돌아서 try_send_one 다시 호출함
12. 8번부터 다시 시작인데 어찌된게 cmd->state에 실행되는 if문 죄다 피하고 return 0이 나옴
    -> (보낼 cmd가 없어서 첫번째 if문에서 그냥 return 0이 나오는 것 같음 - log 안찍어봄)

13. 다 보냈으니 i10_target_try_send도 0을 return하면서 끝남
    -> pending이 false이니 그냥 i10_target_io_work가 끝나버림

14. 이후 4-5에서 예약했던 work가 다시 돌아가는 것 같음, 
15. 근데 이제 여기서 recv_pdu에서 -EAGAIN이 나오는 것 같음
    (보낼게 없으니까?)
16. 이후 try_recv, try_send 죄다 0 return하면서 끝남.

여기까진 준수한데, 다음 data_ready가 굉장히 오래 걸림.
*/


//host에서 socket 쏘면 여기서 socket을 받고 i10_target_queue에 옮기는 것 같음
static void i10_target_data_ready(struct sock *sk)
{
	struct i10_target_queue *queue;

	read_lock_bh(&sk->sk_callback_lock); // 소켓을 읽기 모드로 잠그기
	queue = sk->sk_user_data; // 소켓의 사용자 데이터에서 i10_target_queue 정보 가져옴
	if (likely(queue))
		queue_work_on(queue->cpu, i10_target_wq, &queue->io_work);
	read_unlock_bh(&sk->sk_callback_lock);
}




/*
현재 추측으로는 host에서 caravan을 보내면 target의 worker thread가 wake되어서 이게 돌아가는 것 같음.
request를 가져오는 것 같은데 그럼 caravan 해체는 언제,어디서 하는거지?
*/
static void i10_target_io_work(struct work_struct *w)
{
	struct i10_target_queue *queue =
		container_of(w, struct i10_target_queue, io_work); // 작업을 수행할 queue 가져오고
	bool pending;
	int ret, ops = 0;

	do {
		pending = false;

        //receive budget이랑, send budget은 16으로 aggregation size랑 일치함
		ret = i10_target_try_recv(queue, I10_TARGET_RECV_BUDGET, &ops); // data 수신하는 것 같음?
		if (ret > 0) {
			pending = true;
		} else if (ret < 0) {
			if (ret == -EPIPE || ret == -ECONNRESET)
				kernel_sock_shutdown(queue->sock, SHUT_RDWR);
			else
				i10_target_fatal_error(queue);
			return;
		}

		ret = i10_target_try_send(queue, I10_TARGET_SEND_BUDGET, &ops); // 송신할 데이터 처리
		if (ret > 0) {
			/* transmitted message/data */
			pending = true;
		} else if (ret < 0) {
			if (ret == -EPIPE || ret == -ECONNRESET)
				kernel_sock_shutdown(queue->sock, SHUT_RDWR);
			else
				i10_target_fatal_error(queue);
			return;
		}

	} while (pending && ops < I10_TARGET_IO_WORK_BUDGET);

	/*
	 * We exahusted our budget, requeue our selves
	 */
	if (pending)
		queue_work_on(queue->cpu, i10_target_wq, &queue->io_work);
}


//budget은 I10_TARGET_RECV_BUDGET, int *recvs = &ops <- 그냥 int data, 실제로 수신한 request의 수를 추적
static int i10_target_try_recv(struct i10_target_queue *queue,
		int budget, int *recvs)
{
	int i, ret = 0;

    // budget만큼 request를 receive 하려는 것 같음 - request를 받아오면 ret는 양수가 되겠지?
	for (i = 0; i < budget; i++) {
		ret = i10_target_try_recv_one(queue);
		if (ret <= 0)
			break;
		(*recvs)++; // recv_one 할때마다 ops++ 해줌
	}

	return ret;
}

static int i10_target_try_recv_one(struct i10_target_queue *queue)
{
	int result = 0;
    //rcv_state : 수신 상태를 나타내는 변수겠지?
	if (unlikely(queue->rcv_state == I10_TARGET_RECV_ERR)) // 일단 rcv_state가 error인지 확인하고
		return 0;


    // i10_target_prepare_receive_pdu에서 rcv_state가 I10_TARGET_RECV_PDU로 set되긴 하는데 일단 이거면 recv_pdu 호출
	if (queue->rcv_state == I10_TARGET_RECV_PDU) {
		result = i10_target_try_recv_pdu(queue); // PDU를 수신하고 유효성을 처리한다? (너무 복잡해서 분석 못함 이거는)
		if (result != 0)
			goto done_recv;
	}

	if (queue->rcv_state == I10_TARGET_RECV_DATA) {
		result = i10_target_try_recv_data(queue);
		if (result != 0)
			goto done_recv;
	}

	if (queue->rcv_state == I10_TARGET_RECV_DDGST) {
		result = i10_target_try_recv_ddgst(queue);
		if (result != 0)
			goto done_recv;
	}

done_recv:
	if (result < 0) {
		if (result == -EAGAIN)
			return 0;
		return result;
	}
	return 1;
}


static void i10_target_queue_response(struct nvmet_req *req)
{
	//printk("target get the request \n");
	struct i10_target_cmd *cmd =
		container_of(req, struct i10_target_cmd, req);
	struct i10_target_queue	*queue = cmd->queue;
	struct nvme_sgl_desc *sgl;
	u32 len;

	if (unlikely(cmd == queue->cmd)) {
		sgl = &cmd->req.cmd->common.dptr.sgl;
		len = le32_to_cpu(sgl->length);

		/*
		 * Wait for inline data before processing the response.
		 * Avoid using helpers, this might happen before
		 * nvmet_req_init is completed.
		 */
		if (queue->rcv_state == I10_TARGET_RECV_PDU &&
		    len && len <= cmd->req.port->inline_data_size &&
		    nvme_is_write(cmd->req.cmd))
			return;
	}
	//printk("target wake queue_work_on up\n");
	llist_add(&cmd->lentry, &queue->resp_list);
	queue_work_on(cmd->queue->cpu, i10_target_wq, &cmd->queue->io_work);
}


//budget만큼 data를 전송하는거 같음
//*sends를 통해 ops++해서 data 전송 횟수 관리함
static int i10_target_try_send(struct i10_target_queue *queue,
		int budget, int *sends)
{
	int i, ret = 0;

	for (i = 0; i < budget; i++) { // budget만큼 data 전송
		ret = i10_target_try_send_one(queue, i == budget - 1); // 실제 data 전송, i == budget - 1을 통해서 batch의 마지막 data인지 확인

		/* Send i10 caravans */
        //이게 뭐하는 과정인지 모르겠네? caravan을 보내? host로 보내나?
		if ((queue->send_now || ret <= 0 || i == budget - 1) &&
			queue->caravan_len) {
			struct msghdr msg =
				{ .msg_flags = MSG_DONTWAIT | MSG_EOR };
			int i10_ret, j;

			if (i10_target_sndbuf_nospace(queue,
				queue->caravan_len)) {
				set_bit(SOCK_NOSPACE,
					&queue->sock->sk->sk_socket->flags);
				return 0;
			}

			i10_ret = kernel_sendmsg(queue->sock, &msg,
					queue->caravan_iovs,
					queue->nr_iovs, queue->caravan_len);
			if (unlikely(i10_ret <= 0))
				pr_err("I10_TARGET: kernel_sendmsg fails (i10_ret %d)\n",
					i10_ret);

			for (j = 0; j < queue->nr_caravan_cmds; j++) {
				kfree(queue->caravan_cmds[j].cmd->iov);
				sgl_free(queue->caravan_cmds[j].cmd->req.sg);
				i10_target_put_cmd(queue->caravan_cmds[j].cmd);
			}

			for (j = 0; j < queue->nr_caravan_mapped; j++)
				kunmap(queue->caravan_mapped[j]);

			queue->nr_iovs = 0;
			queue->nr_caravan_cmds = 0;
			queue->nr_caravan_mapped = 0;
			queue->caravan_len = 0;
			queue->send_now = false;
		}

		if (ret <= 0)
			break;
		(*sends)++;
	}
	return ret;
}



static void i10_target_queue_response(struct nvmet_req *req)
{
	struct i10_target_cmd *cmd =
		container_of(req, struct i10_target_cmd, req);
	struct i10_target_queue	*queue = cmd->queue;
	struct nvme_sgl_desc *sgl;
	u32 len;

	if (unlikely(cmd == queue->cmd)) {
		sgl = &cmd->req.cmd->common.dptr.sgl;
		len = le32_to_cpu(sgl->length);

		/*
		 * Wait for inline data before processing the response.
		 * Avoid using helpers, this might happen before
		 * nvmet_req_init is completed.
		 */
		if (queue->rcv_state == I10_TARGET_RECV_PDU &&
		    len && len <= cmd->req.port->inline_data_size &&
		    nvme_is_write(cmd->req.cmd))
			return;
	}

	llist_add(&cmd->lentry, &queue->resp_list);
	queue_work_on(cmd->queue->cpu, i10_target_wq, &cmd->queue->io_work);
}
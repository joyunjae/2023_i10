/*
1. i10_host_queue 구조체란?
*/

//nvme_tcp_queue처럼 생겼으나,
//nvme에서는 mutex를 썼는데 -> spinlock을 씀
struct i10_host_queue {
	struct socket		*sock;
	struct work_struct	io_work;
	int			io_cpu;

	spinlock_t		lock;
	struct list_head	send_list;

	/* recv state */
	void			*pdu;
	int			pdu_remaining;
	int			pdu_offset;
	size_t			data_remaining;
	size_t			ddgst_remaining;

	/* send state */
	struct i10_host_request *request; // 현재 송신중인 request를 가리킬 pointer

	int			queue_size;
	size_t			cmnd_capsule_len;
	struct i10_host_ctrl	*ctrl;
	unsigned long		flags;
	bool			rd_enabled;

	bool			hdr_digest;
	bool			data_digest;
	struct ahash_request	*rcv_hash;
	struct ahash_request	*snd_hash;
	__le32			exp_ddgst;
	__le32			recv_ddgst;

	/* For i10 caravans */
	struct kvec		*caravan_iovs;
	size_t			caravan_len;
	int			nr_iovs;
	bool			send_now;

	struct page		**caravan_mapped;
	int			nr_mapped;

	/* For i10 delayed doorbells */
	int			nr_req;
	struct hrtimer		doorbell_timer;

	struct page_frag_cache	pf_cache;

	void (*state_change)(struct sock *);
	void (*data_ready)(struct sock *);
	void (*write_space)(struct sock *);
};


/*
2. i10_host_request 구조체란?
*/

//nvme_tcp_request랑 비슷하게 생겼음.
//block I/O request를 tcp network 프로토콜에 맞추어 처리하기 위한 구조체인데, i10을 위한 구조체로 바꾼거겠지
struct i10_host_request {
	struct nvme_request	req;
	void			*pdu;
	struct i10_host_queue	*queue; // 해당 request가 속한 queue pointer겠지
	u32			data_len;
	u32			pdu_len;
	u32			pdu_sent;
	u16			ttag;
	struct list_head	entry;
	__le32			ddgst;

	struct bio		*curr_bio;
	struct iov_iter		iter;

	/* send state */
	size_t			offset;
	size_t			data_sent;
	enum i10_host_send_state state;
};

/*
3. i10_host_ctrl 구조체란?
*/

//host i10을 전체적으로 관리하는데 사용되는 구조체인 것으로 확인된다.
struct i10_host_ctrl {
	/* read only in the hot path */
	struct i10_host_queue	*queues; // i10 queue들을 가리키는 포인터 배열인듯?
	struct blk_mq_tag_set	tag_set;

	/* other member variables */
	struct list_head	list;
	struct blk_mq_tag_set	admin_tag_set;
	struct sockaddr_storage addr; // 소켓 연결시 사용되는 주소
	struct sockaddr_storage src_addr;
	struct nvme_ctrl	ctrl;

	struct work_struct	err_work;
	struct delayed_work	connect_work;
	struct i10_host_request async_req;
};

/*
4. hrtimer 구조체란?
고해상도 타이머(hrtimer)의 기본적인 데이터 구조체

The hrtimer structure must be initialized by hrtimer_init()
*/
struct hrtimer {
	struct timerqueue_node		node; // 내부적으로 타이머의 만료 시간 관리
	ktime_t				_softexpires; // 타이머가 설정될 때 지정한 만료 시간을 나타냄
	enum hrtimer_restart		(*function)(struct hrtimer *); // 타이머 만료 시 호출되는 콜백 함수 포인터, 이 콜백 함수는 enum hrtimer_restart 타입을 반환해야 하며, 리턴 값에 따라 타이머 동작이 결정됩니다.
	struct hrtimer_clock_base	*base; // 타이머가 속한 타이머 베이스(clock base)를 가리키는 포인터입니다. 타이머 베이스는 특정 CPU와 클럭에 대한 타이머 정보와 동작을 담당합니다.
	u8				state; // 타이머의 현재 상태 정보
	u8				is_rel;
	u8				is_soft;
	u8				is_hard;
};
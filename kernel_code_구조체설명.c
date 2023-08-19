1. blk_status_t란?
blk_status_t는 Linux 커널의 블록 I/O 계층에서 사용되는 데이터 타입으로,
블록 장치 드라이버나 파일 시스템과 같은 여러 커널 모듈에서 I/O 요청 처리 중 발생하는 결과를 나타내기 위해 사용됩니다. 
이 데이터 타입은 I/O 요청의 성공, 실패 또는 다양한 에러 상태를 나타내는 데에 활용됩니다.

blk_status_t는 다양한 미리 정의된 상수값을 통해 I/O 요청의 상태를 나타내며, 주요한 몇 가지 값은 아래와 같습니다.

BLK_STS_OK: I/O 요청이 성공적으로 처리되었음을 나타냅니다.
BLK_STS_NOTSUPP: 해당 연산이 장치에 의해 지원되지 않는다는 것을 나타냅니다.
BLK_STS_IOERR: I/O 에러가 발생했음을 나타냅니다.
BLK_STS_NOSPC: 공간 부족으로 인해 I/O 요청을 처리할 수 없음을 나타냅니다.
BLK_STS_DEVERR: 블록 장치에 문제가 발생하여 I/O 처리가 불가능함을 나타냅니다.

2. struct blk_mq_hw_ctx *hctx란?
struct blk_mq_hw_ctx *hctx는 블록 다중 대기열 시스템에서 현재 I/O 요청을 처리할 하드웨어 컨텍스트를 나타냅니다. 
이 hctx 매개변수는 해당 I/O 요청을 어떤 큐와 어떤 CPU 코어나 스레드에서 처리할지를 결정하는 데 사용됩니다.




3. blk_mq_queue_data *bd란?
Block I/O request에 대한 정보를 보유하는 구조체
Block I/O request를 처리하거나 큐에 추가할 때 사용된다.

struct blk_mq_queue_data {
	struct request *rq; // Block I/O request를 가리키는 pointer
	bool last; // queue에 추가되는 block I/O request가 해당 queue의 마지막 element인지 확인
};


4. struct nvme_ns란?
struct nvme_ns 구조체는 NVMe 스토리지 디바이스의 각 네임스페이스에 대한 정보를 저장합니다. 
이 구조체는 해당 네임스페이스의 속성, 용량, 상태 등을 포함하며, 블록 I/O 드라이버나 NVMe 컨트롤러에서 네임스페이스와 관련된 작업을 수행할 때 사용됩니다.


5. struct request란?
struct request는 리눅스 커널 내에서 블록 I/O 요청을 나타내기 위한 구조체입니다. 
블록 I/O 요청은 하드 디스크, SSD 및 기타 블록 기반 스토리지 장치에 대한 데이터 읽기 및 쓰기와 같은 작업을 나타냅니다. 
이 구조체는 I/O 요청을 추상화하여 스토리지 장치와 상호작용하고 처리하는 데 사용됩니다.


struct request {
	struct request_queue *q;
	struct blk_mq_ctx *mq_ctx;
	struct blk_mq_hw_ctx *mq_hctx;

	unsigned int cmd_flags;		/* op and common flags */
	req_flags_t rq_flags;

	int tag;
	int internal_tag;

	/* the following two fields are internal, NEVER access directly */
	unsigned int __data_len;	/* total data len */
	sector_t __sector;		/* sector cursor */

	struct bio *bio;
	struct bio *biotail;

	struct list_head queuelist;

	/*
	 * The hash is used inside the scheduler, and killed once the
	 * request reaches the dispatch list. The ipi_list is only used
	 * to queue the request for softirq completion, which is long
	 * after the request has been unhashed (and even removed from
	 * the dispatch list).
	 */
	union {
		struct hlist_node hash;	/* merge hash */
		struct list_head ipi_list;
	};

	/*
	 * The rb_node is only used inside the io scheduler, requests
	 * are pruned when moved to the dispatch queue. So let the
	 * completion_data share space with the rb_node.
	 */
	union {
		struct rb_node rb_node;	/* sort/lookup */
		struct bio_vec special_vec;
		void *completion_data;
		int error_count; /* for legacy drivers, don't use */
	};

	/*
	 * Three pointers are available for the IO schedulers, if they need
	 * more they have to dynamically allocate it.  Flush requests are
	 * never put on the IO scheduler. So let the flush fields share
	 * space with the elevator data.
	 */
	union {
		struct {
			struct io_cq		*icq;
			void			*priv[2];
		} elv;

		struct {
			unsigned int		seq;
			struct list_head	list;
			rq_end_io_fn		*saved_end_io;
		} flush;
	};

	struct gendisk *rq_disk;
	struct hd_struct *part;
#ifdef CONFIG_BLK_RQ_ALLOC_TIME
	/* Time that the first bio started allocating this request. */
	u64 alloc_time_ns;
#endif
	/* Time that this request was allocated for this IO. */
	u64 start_time_ns;
	/* Time that I/O was submitted to the device. */
	u64 io_start_time_ns;

#ifdef CONFIG_BLK_WBT
	unsigned short wbt_flags;
#endif
	/*
	 * rq sectors used for blk stats. It has the same value
	 * with blk_rq_sectors(rq), except that it never be zeroed
	 * by completion.
	 */
	unsigned short stats_sectors;

	/*
	 * Number of scatter-gather DMA addr+len pairs after
	 * physical address coalescing is performed.
	 */
	unsigned short nr_phys_segments;

#if defined(CONFIG_BLK_DEV_INTEGRITY)
	unsigned short nr_integrity_segments;
#endif

#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	struct bio_crypt_ctx *crypt_ctx;
	struct blk_ksm_keyslot *crypt_keyslot;
#endif

	unsigned short write_hint;
	unsigned short ioprio;

	enum mq_rq_state state;
	refcount_t ref;

	unsigned int timeout;
	unsigned long deadline;

	union {
		struct __call_single_data csd;
		u64 fifo_time;
	};

	/*
	 * completion callback.
	 */
	rq_end_io_fn *end_io;
	void *end_io_data;
};


6. struct nvme_request란?
NVMe storage device에 대한 block I/O request를 나타내는 구조체
NVMe 컨트롤러와 상호작용하며 block I/O 작업을 관리, 명령 실행 결과에 대한 정보를 담고 있음.
struct nvme_request {
	struct nvme_command	*cmd; // NVMe 명령을 나타내는 구조체 포인터
	union nvme_result	result;
	u8			genctr;
	u8			retries;
	u8			flags;
	u16			status;
	struct nvme_ctrl	*ctrl; // NVMe 컨트롤러를 가리킴. -> 해당 reqeust가 속한 컨트롤러에 대한 정보를 담고있음.
};


7. struct nvme_tcp_request란?
request의 메타데이터와 전송 상태를 기록, block I/O request를 TCP 네트워크 프로토콜에 맞추어 처리하는데 사용되는 구조체

struct nvme_tcp_request {
	struct nvme_request	req; // NVMe 명령
	void			*pdu; // NVMe-oF 프로토콜 데이터 전송 단위
	struct nvme_tcp_queue	*queue; // 해당 request가 속한 TCP queue를 가리킴
	u32			data_len;
	u32			pdu_len;
	u32			pdu_sent;
	u16			ttag;
	struct list_head	entry;
	struct llist_node	lentry;
	__le32			ddgst;

	struct bio		*curr_bio; // 현재 처리 중인 Block I/O를 가리키는 포인터
	struct iov_iter		iter;

	/* send state */
	size_t			offset;
	size_t			data_sent;
	enum nvme_tcp_send_state state; // TCP 전송 상태
};


8. struct nvme_tcp_queue란?
NVMe-oF TCP 프로토콜에서 사용되는 TCP 소켓 및 데이터 전송 상태를 관리하기 위한 구조체

struct nvme_tcp_queue {
	struct socket		*sock; // TCP 소켓을 나타내는 포인터
	struct work_struct	io_work;
	int			io_cpu; // I/O 작업을 처리할 CPU코어의 index

	struct mutex		queue_lock; // NVMe-oF queue에 대한 mutex 잠금 제공, queue 접근 동기화
	struct mutex		send_mutex;
	struct llist_head	req_list; // NVMe-oF queue에 대기중인 request들을 Lockless 연결리스트로 관리
	//nvme_tcp_queue_request에서 추가되고, nvme_tcp_try_send에서 request를 처리하면서 사용된다.
	
	struct list_head	send_list; // 현재 전송 중인 요청들을 표준 이중 연결리스트로 관리
	//nvme_tcp_try_send에서 전송 중인 요청을 관리하는데 사용된다.

	bool			more_requests;

	/* recv state */
	void			*pdu;
	int			pdu_remaining;
	int			pdu_offset;
	size_t			data_remaining;
	size_t			ddgst_remaining;
	unsigned int		nr_cqe;

	/* send state */
	struct nvme_tcp_request *request;

	int			queue_size;
	size_t			cmnd_capsule_len;
	struct nvme_tcp_ctrl	*ctrl;
	unsigned long		flags;
	bool			rd_enabled;

	bool			hdr_digest;
	bool			data_digest;
	struct ahash_request	*rcv_hash;
	struct ahash_request	*snd_hash;
	__le32			exp_ddgst;
	__le32			recv_ddgst;

	struct page_frag_cache	pf_cache;

	void (*state_change)(struct sock *);
	void (*data_ready)(struct sock *);
	void (*write_space)(struct sock *);
};


9. struct nvme_tcp_cmd_pdu 구조체
이 구조체는 NVMe 컨트롤러로 전송될 커맨드를 표현하기 위해 사용됩니다.

struct nvme_tcp_cmd_pdu {
	struct nvme_tcp_hdr	hdr; //커맨드 PDU의 헤더 정보를 담는 구조체입니다. NVMe-oF 프로토콜의 헤더 필드들이 포함되어 있습니다.
	struct nvme_command	cmd; //NVMe 컨트롤러에 전송될 커맨드 정보를 담는 구조체입니다. 이는 실제로 NVMe SSD 디바이스에 대한 명령을 나타냅니다.
};


10. nvme_tcp_ctrl 구조체 설명
NVMe-oF 드라이버에서 사용되는 컨트롤러 정보를 나타내는 구조체

struct nvme_tcp_ctrl {
	/* read only in the hot path */
	struct nvme_tcp_queue	*queues; // NVMe-oF 컨트롤러에 연결된 여러 개의 queue를 가리키는 포인터 배열
	struct blk_mq_tag_set	tag_set;

	/* other member variables */
	struct list_head	list; // NVMe-oF 컨트롤러들의 리스트를 연결하는 링크드 리스트라는데
	struct blk_mq_tag_set	admin_tag_set;
	struct sockaddr_storage addr;
	struct sockaddr_storage src_addr;
	struct nvme_ctrl	ctrl;

	struct work_struct	err_work;
	struct delayed_work	connect_work;
	struct nvme_tcp_request async_req;
	u32			io_queues[HCTX_MAX_TYPES];
};
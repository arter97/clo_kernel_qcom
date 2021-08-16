#ifndef _MHI_EP_INTERNAL_
#define _MHI_EP_INTERNAL_

#include <linux/bitfield.h>
#include <linux/mhi.h>
#include <linux/types.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>

extern struct bus_type mhi_ep_bus_type;

/* MHI register definition */
#define MHI_CTRL_INT_STATUS_A7		0x4
#define MHI_CHDB_INT_STATUS_A7_n(n)	(0x28 + 0x4 * (n))
#define MHI_ERDB_INT_STATUS_A7_n(n)	(0x38 + 0x4 * (n))

#define MHI_CTRL_INT_CLEAR_A7		0x4c
#define MHI_CTRL_INT_MMIO_WR_CLEAR	BIT(2)
#define MHI_CTRL_INT_CRDB_CLEAR		BIT(1)
#define MHI_CTRL_INT_CRDB_MHICTRL_CLEAR	BIT(0)

#define MHI_CHDB_INT_CLEAR_A7_n(n)	(0x70 + 0x4 * (n))
#define MHI_CHDB_INT_CLEAR_A7_n_CLEAR_ALL GENMASK(31, 0)
#define MHI_ERDB_INT_CLEAR_A7_n(n)	(0x80 + 0x4 * (n))
#define MHI_ERDB_INT_CLEAR_A7_n_CLEAR_ALL GENMASK(31, 0)

#define MHI_CTRL_INT_MASK_A7		0x94
#define MHI_CTRL_INT_MASK_A7_MASK_MASK	GENMASK(1, 0)
#define MHI_CTRL_MHICTRL_MASK		BIT(0)
#define MHI_CTRL_MHICTRL_SHFT		0
#define MHI_CTRL_CRDB_MASK		BIT(1)
#define MHI_CTRL_CRDB_SHFT		1

#define MHI_CHDB_INT_MASK_A7_n(n)	(0xb8 + 0x4 * (n))
#define MHI_CHDB_INT_MASK_A7_n_EN_ALL	GENMASK(31, 0) 
#define MHI_ERDB_INT_MASK_A7_n(n)	(0xc8 + 0x4 * (n))
#define MHI_ERDB_INT_MASK_A7_n_EN_ALL	GENMASK(31, 0) 

#define MHIREGLEN			0x100
#define MHIVER				0x108

#define MHICFG				0x110
#define MHICFG_NHWER_MASK		GENMASK(31, 24)
#define MHICFG_NER_MASK			GENMASK(23, 16)
#define MHICFG_RESERVED_BITS15_8_MASK	GENMASK(15, 8)
#define MHICFG_NCH_MASK			GENMASK(7, 0)

#define CHDBOFF				0x118
#define ERDBOFF				0x120
#define BHIOFF				0x128
#define DEBUGOFF			0x130

#define MHICTRL				0x138
#define MHICTRL_MHISTATE_MASK		GENMASK(15, 8)
#define MHICTRL_RESET_MASK		BIT(1)
#define MHICTRL_RESET_SHIFT		1

#define MHISTATUS			0x148
#define MHISTATUS_MHISTATE_MASK		GENMASK(15, 8)
#define MHISTATUS_MHISTATE_SHIFT	8
#define MHISTATUS_SYSERR_MASK		BIT(2)
#define MHISTATUS_SYSERR_SHIFT		2
#define MHISTATUS_READY_MASK		BIT(0)
#define MHISTATUS_READY_SHIFT		0

#define CCABAP_LOWER			0x158
#define CCABAP_HIGHER			0x15C
#define ECABAP_LOWER			0x160
#define ECABAP_HIGHER			0x164
#define CRCBAP_LOWER			0x168
#define CRCBAP_HIGHER			0x16C
#define CRDB_LOWER			0x170
#define CRDB_HIGHER			0x174
#define MHICTRLBASE_LOWER		0x180
#define MHICTRLBASE_HIGHER		0x184
#define MHICTRLLIMIT_LOWER		0x188
#define MHICTRLLIMIT_HIGHER		0x18C
#define MHIDATABASE_LOWER		0x198
#define MHIDATABASE_HIGHER		0x19C
#define MHIDATALIMIT_LOWER		0x1A0
#define MHIDATALIMIT_HIGHER		0x1A4
#define CHDB_LOWER_n(n)			(0x400 + 0x8 * (n))
#define CHDB_HIGHER_n(n)		(0x404 + 0x8 * (n))
#define ERDB_LOWER_n(n)			(0x800 + 0x8 * (n))
#define ERDB_HIGHER_n(n)		(0x804 + 0x8 * (n))
#define BHI_INTVEC			0x220
#define BHI_EXECENV			0x228
#define BHI_IMGTXDB			0x218

#define NR_OF_CMD_RINGS			1
#define NUM_EVENT_RINGS			128
#define NUM_HW_EVENT_RINGS		2
#define NUM_CHANNELS			128
#define HW_CHANNEL_BASE			100
#define NUM_HW_CHANNELS			15
#define HW_CHANNEL_END			110
#define MHI_ENV_VALUE			2
#define MHI_MASK_ROWS_CH_EV_DB		4
#define TRB_MAX_DATA_SIZE		8192
#define MHI_CTRL_STATE			100

#define MHI_NET_DEFAULT_MTU		8192

struct mhi_ep_chan;
extern struct bus_type mhi_ep_bus_type;

enum cb_reason {
	MHI_EP_TRE_AVAILABLE = 0,
	MHI_EP_CTRL_UPDATE,
};

enum mhi_ep_ctrl_info {
	MHI_EP_STATE_CONFIGURED,
	MHI_EP_STATE_CONNECTED,
	MHI_EP_STATE_DISCONNECTED,
	MHI_EP_STATE_INVAL,
};

#if 0
/* Channel state */
enum mhi_ep_ch_state {
	MHI_EP_CH_STATE_DISABLED,
	MHI_EP_CH_STATE_ENABLED,
	MHI_EP_CH_STATE_RUNNING,
	MHI_EP_CH_STATE_SUSPENDED,
	MHI_EP_CH_STATE_STOP,
	MHI_EP_CH_STATE_ERROR,
};

#define CHAN_CTX_CHSTATE_MASK GENMASK(7, 0)
#define CHAN_CTX_CHSTATE_SHIFT 0
#define CHAN_CTX_BRSTMODE_MASK GENMASK(9, 8)
#define CHAN_CTX_BRSTMODE_SHIFT 8
#define CHAN_CTX_POLLCFG_MASK GENMASK(15, 10)
#define CHAN_CTX_POLLCFG_SHIFT 10
#define CHAN_CTX_RESERVED_MASK GENMASK(31, 16)
struct mhi_ep_ch_ctx {
	__u32 chcfg;
	__u32 chtype;
	__u32 erindex;

	__u64 rbase __packed __aligned(4);
	__u64 rlen __packed __aligned(4);
	__u64 rp __packed __aligned(4);
	__u64 wp __packed __aligned(4);
};
#endif
/* Channel context state */
enum mhi_ep_ch_ctx_state {
	MHI_EP_CH_STATE_DISABLED,
	MHI_EP_CH_STATE_ENABLED,
	MHI_EP_CH_STATE_RUNNING,
	MHI_EP_CH_STATE_SUSPENDED,
	MHI_EP_CH_STATE_STOP,
	MHI_EP_CH_STATE_ERROR,
	MHI_EP_CH_STATE_RESERVED,
	MHI_EP_CH_STATE_32BIT = 0x7FFFFFFF
};

/* Channel type */
enum mhi_ep_ch_ctx_type {
	MHI_EP_CH_TYPE_NONE,
	MHI_EP_CH_TYPE_OUTBOUND_CHANNEL,
	MHI_EP_CH_TYPE_INBOUND_CHANNEL,
	MHI_EP_CH_RESERVED
};

struct mhi_ep_ch_ctx {
	enum mhi_ep_ch_ctx_state	ch_state;
	enum mhi_ep_ch_ctx_type	ch_type;
	uint32_t			err_indx;
	uint64_t			rbase;
	uint64_t			rlen;
	uint64_t			rp;
	uint64_t			wp;
} __packed;

/* Event context interrupt moderation */
enum mhi_ep_evt_ctx_int_mod_timer {
	MHI_EP_EVT_INT_MODERATION_DISABLED
};

/* Event ring type */
enum mhi_ep_evt_ctx_event_ring_type {
	MHI_EP_EVT_TYPE_DEFAULT,
	MHI_EP_EVT_TYPE_VALID,
	MHI_EP_EVT_RESERVED
};

#if 0
/* Event ring context type */
#define EV_CTX_RESERVED_MASK GENMASK(7, 0)
#define EV_CTX_INTMODC_MASK GENMASK(15, 8)
#define EV_CTX_INTMODC_SHIFT 8
#define EV_CTX_INTMODT_MASK GENMASK(31, 16)
#define EV_CTX_INTMODT_SHIFT 16
struct mhi_ep_ev_ctx {
	__u32 intmod;
	__u32 ertype;
	__u32 msivec;

	__u64 rbase __packed __aligned(4);
	__u64 rlen __packed __aligned(4);
	__u64 rp __packed __aligned(4);
	__u64 wp __packed __aligned(4);
};

/* Command context */
struct mhi_ep_cmd_ctx {
	__u32 reserved0;
	__u32 reserved1;
	__u32 reserved2;

	__u64 rbase __packed __aligned(4);
	__u64 rlen __packed __aligned(4);
	__u64 rp __packed __aligned(4);
	__u64 wp __packed __aligned(4);
};

/* generic context */
struct mhi_ep_gen_ctx {
	__u32 reserved0;
	__u32 reserved1;
	__u32 reserved2;

	__u64 rbase __packed __aligned(4);
	__u64 rlen __packed __aligned(4);
	__u64 rp __packed __aligned(4);
	__u64 wp __packed __aligned(4);
};
#endif

/* Event ring context type */
struct mhi_ep_ev_ctx {
	uint32_t				res1:16;
	enum mhi_ep_evt_ctx_int_mod_timer	intmodt:16;
	enum mhi_ep_evt_ctx_event_ring_type	ertype;
	uint32_t				msivec;
	uint64_t				rbase;
	uint64_t				rlen;
	uint64_t				rp;
	uint64_t				wp;
} __packed;

/* Command context */
struct mhi_ep_cmd_ctx {
	uint32_t				res1;
	uint32_t				res2;
	uint32_t				res3;
	uint64_t				rbase;
	uint64_t				rlen;
	uint64_t				rp;
	uint64_t				wp;
} __packed;

/* generic context */
struct mhi_ep_gen_ctx {
	uint32_t				res1;
	uint32_t				res2;
	uint32_t				res3;
	uint64_t				rbase;
	uint64_t				rlen;
	uint64_t				rp;
	uint64_t				wp;
} __packed;

enum mhi_ep_ring_element_type_id {
	MHI_EP_RING_EL_INVALID = 0,
	MHI_EP_RING_EL_NOOP = 1,
	MHI_EP_RING_EL_TRANSFER = 2,
	MHI_EP_RING_EL_RESET = 16,
	MHI_EP_RING_EL_STOP = 17,
	MHI_EP_RING_EL_START = 18,
	MHI_EP_RING_EL_MHI_STATE_CHG = 32,
	MHI_EP_RING_EL_CMD_COMPLETION_EVT = 33,
	MHI_EP_RING_EL_TRANSFER_COMPLETION_EVENT = 34,
	MHI_EP_RING_EL_EE_STATE_CHANGE_NOTIFY = 64,
	MHI_EP_RING_EL_UNDEF
};

enum mhi_ep_ring_state {
	RING_STATE_UINT = 0,
	RING_STATE_IDLE,
	RING_STATE_PENDING,
};

enum mhi_ep_ring_type {
	RING_TYPE_CMD = 0,
	RING_TYPE_ER,
	RING_TYPE_CH,
	RING_TYPE_INVALID,
};

/* Transfer ring element */
struct mhi_ep_transfer_ring_element {
	u64				data_buf_ptr;
	u32				len:16;
	u32				res1:16;
	u32				chain:1;
	u32				res2:7;
	u32				ieob:1;
	u32				ieot:1;
	u32				bei:1;
	u32				res3:5;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				res4:8;
} __packed;

/* Command ring element */
/* Command ring No op command */
struct mhi_ep_cmd_ring_op {
	u64				res1;
	u32				res2;
	u32				res3:16;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				chid:8;
} __packed;

/* Command ring reset channel command */
struct mhi_ep_cmd_ring_reset_channel_cmd {
	u64				res1;
	u32				res2;
	u32				res3:16;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				chid:8;
} __packed;

/* Command ring stop channel command */
struct mhi_ep_cmd_ring_stop_channel_cmd {
	u64				res1;
	u32				res2;
	u32				res3:16;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				chid:8;
} __packed;

/* Command ring start channel command */
struct mhi_ep_cmd_ring_start_channel_cmd {
	u64				res1;
	u32				seqnum;
	u32				reliable:1;
	u32				res2:15;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				chid:8;
} __packed;

enum mhi_ep_cmd_completion_code {
	MHI_CMD_COMPL_CODE_INVALID = 0,
	MHI_CMD_COMPL_CODE_SUCCESS = 1,
	MHI_CMD_COMPL_CODE_EOT = 2,
	MHI_CMD_COMPL_CODE_OVERFLOW = 3,
	MHI_CMD_COMPL_CODE_EOB = 4,
	MHI_CMD_COMPL_CODE_UNDEFINED = 16,
	MHI_CMD_COMPL_CODE_RING_EL = 17,
	MHI_CMD_COMPL_CODE_RES
};

/* Event ring elements */
/* Transfer completion event */
struct mhi_ep_event_ring_transfer_completion {
	u64				ptr;
	u32				len:16;
	u32				res1:8;
	enum mhi_ep_cmd_completion_code	code:8;
	u32				res2:16;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				chid:8;
} __packed;

/* Command completion event */
struct mhi_ep_event_ring_cmd_completion {
	u64				ptr;
	u32				res1:24;
	enum mhi_ep_cmd_completion_code	code:8;
	u32				res2:16;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				res3:8;
} __packed;

/**
 * enum mhi_ep_event_type - MHI state change events
 * @MHI_EP_EVENT_CTRL_TRIG: CTRL register change event.
 *				Not supported,for future use
 * @MHI_EP_EVENT_M0_STATE: M0 state change event
 * @MHI_EP_EVENT_M1_STATE: M1 state change event. Not supported, for future use
 * @MHI_EP_EVENT_M2_STATE: M2 state change event. Not supported, for future use
 * @MHI_EP_EVENT_M3_STATE: M0 state change event
 * @MHI_EP_EVENT_HW_ACC_WAKEUP: pendding data on IPA, initiate Host wakeup
 * @MHI_EP_EVENT_CORE_WAKEUP: MHI core initiate Host wakup
 */
enum mhi_ep_event_type {
	MHI_EP_EVENT_CTRL_TRIG,
	MHI_EP_EVENT_M0_STATE,
	MHI_EP_EVENT_M1_STATE,
	MHI_EP_EVENT_M2_STATE,
	MHI_EP_EVENT_M3_STATE,
	MHI_EP_EVENT_HW_ACC_WAKEUP,
	MHI_EP_EVENT_CORE_WAKEUP,
	MHI_EP_EVENT_MAX
};

enum mhi_ep_state {
	MHI_EP_RESET_STATE = 0,
	MHI_EP_READY_STATE,
	MHI_EP_M0_STATE,
	MHI_EP_M1_STATE,
	MHI_EP_M2_STATE,
	MHI_EP_M3_STATE,
	MHI_EP_MAX_STATE,
	MHI_EP_SYSERR_STATE = 0xff
};

enum mhi_ep_pcie_state {
	MHI_EP_PCIE_LINK_DISABLE,
	MHI_EP_PCIE_D0_STATE,
	MHI_EP_PCIE_D3_HOT_STATE,
	MHI_EP_PCIE_D3_COLD_STATE,
};

enum mhi_ep_pcie_event {
	MHI_EP_PCIE_EVENT_INVALID = 0,
	MHI_EP_PCIE_EVENT_PM_D0 = 0x1,
	MHI_EP_PCIE_EVENT_PM_D3_HOT = 0x2,
	MHI_EP_PCIE_EVENT_PM_D3_COLD = 0x4,
	MHI_EP_PCIE_EVENT_PM_RST_DEAST = 0x8,
	MHI_EP_PCIE_EVENT_LINKDOWN = 0x10,
	MHI_EP_PCIE_EVENT_LINKUP = 0x20,
	MHI_EP_PCIE_EVENT_MHI_A7 = 0x40,
	MHI_EP_PCIE_EVENT_MMIO_WRITE = 0x80,
	MHI_EP_PCIE_EVENT_L1SUB_TIMEOUT = 0x100,
	MHI_EP_PCIE_EVENT_L1SUB_TIMEOUT_EXIT = 0x200,
};

/* MHI state change event */
struct mhi_ep_event_ring_state_change {
	u64				ptr;
	u32				res1:24;
	enum mhi_ep_state			mhistate:8;
	u32				res2:16;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				res3:8;
} __packed;

enum mhi_ep_execenv {
	MHI_EP_SBL_EE = 1,
	MHI_EP_AMSS_EE = 2,
	MHI_EP_UNRESERVED
};

/* EE state change event */
struct mhi_ep_event_ring_ee_state_change {
	u64				ptr;
	u32				res1:24;
	enum mhi_ep_execenv			execenv:8;
	u32				res2:16;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				res3:8;
} __packed;

/* Generic cmd to parse common details like type and channel id */
struct mhi_ep_ring_generic {
	u64				ptr;
	u32				res1:24;
	enum mhi_ep_state			mhistate:8;
	u32				res2:16;
	enum mhi_ep_ring_element_type_id	type:8;
	u32				chid:8;
} __packed;

/* Possible ring element types */
union mhi_ep_ring_element_type {
	struct mhi_ep_cmd_ring_op			cmd_no_op;
	struct mhi_ep_cmd_ring_reset_channel_cmd	cmd_reset;
	struct mhi_ep_cmd_ring_stop_channel_cmd	cmd_stop;
	struct mhi_ep_cmd_ring_start_channel_cmd	cmd_start;
	struct mhi_ep_transfer_ring_element		tre;
	struct mhi_ep_event_ring_transfer_completion	evt_tr_comp;
	struct mhi_ep_event_ring_cmd_completion	evt_cmd_comp;
	struct mhi_ep_event_ring_state_change		evt_state_change;
	struct mhi_ep_event_ring_ee_state_change	evt_ee_state;
	struct mhi_ep_ring_generic			generic;
};

/* Transfer ring element type */
union mhi_ep_ring_ctx {
	struct mhi_ep_cmd_ctx cmd;
	struct mhi_ep_ev_ctx ev;
	struct mhi_ep_ch_ctx ch;
	struct mhi_ep_gen_ctx generic;
};

enum mhi_ep_tr_compl_evt_type {
	SEND_EVENT_BUFFER,
	SEND_EVENT_RD_OFFSET,
	SEND_MSI
};

struct mhi_ep_ring {
	enum mhi_ep_ring_type			type;
	enum mhi_ep_ring_state			state;

	u32				ch_id;
	u32				db_offset_h;
	u32				db_offset_l;
	size_t				rd_offset;
	size_t				wr_offset;
	size_t				ring_size;
	struct list_head list;
	struct mhi_ep_cntrl *mhi_cntrl;

	/*
	 * Lock to prevent race in updating event ring
	 * which is shared by multiple channels
	 */
	struct mutex	event_lock;
	/* Physical address of the cached ring copy on the device side */
	dma_addr_t				ring_cache_dma_handle;
	/* Device VA of read pointer array (used only for event rings) */
	u64			*evt_rp_cache;
	/* PA of the read pointer array (used only for event rings) */
	dma_addr_t				evt_rp_cache_dma_handle;
	/* Ring type - cmd, event, transfer ring and its rp/wp... */
	union mhi_ep_ring_ctx			*ring_ctx;
	/* ring_ctx_shadow -> tracking ring_ctx in the host */
	union mhi_ep_ring_ctx			*ring_ctx_shadow;
	int (*ring_cb)(struct mhi_ep_ring *ring, union mhi_ep_ring_element_type *el);
	/* device virtual address location of the cached host ring ctx data */
	union mhi_ep_ring_element_type		*ring_cache;
	/* Copy of the host ring */
	union mhi_ep_ring_element_type		*ring_shadow;
	phys_addr_t				ring_shadow_phys;
};

struct mhi_ep_cmd {
	struct mhi_ep_ring ring;
};

struct mhi_ep_event {
	struct mhi_ep_ring ring;
	spinlock_t lock;
};

static inline void mhi_ep_ring_inc_index(struct mhi_ep_ring *ring,
						size_t rd_offset)
{
	ring->rd_offset++;
	if (ring->rd_offset == ring->ring_size)
		ring->rd_offset = 0;
}

/* trace information planned to use for read/write */
#define TRACE_DATA_MAX				128
#define MHI_EP_DATA_MAX			512

#define MHI_EP_MMIO_RANGE			0xb80
#define MHI_EP_MMIO_OFFSET			0x100

struct ring_cache_req {
	struct completion	*done;
	void			*context;
};

struct event_req {
	union mhi_ep_ring_element_type *tr_events;
	/*
	 * Start index of the completion event buffer segment
	 * to be flushed to host
	 */
	u32			start;
	u32			num_events;
	dma_addr_t		dma;
	u32			dma_len;
	dma_addr_t		event_rd_dma;
	void			*context;
	enum mhi_ep_tr_compl_evt_type event_type;
	u32			event_ring;
	void			(*client_cb)(void *req);
	void			(*rd_offset_cb)(void *req);
	void			(*msi_cb)(void *req);
	struct list_head	list;
	u32			flush_num;
};

/**
 * struct mhi_ep_sm - MHI state manager context information
 * @mhi: TODO 
 * @lock: mutex for mhi_state
 * @wq: workqueue for state change events
 * @work: 
 * @state: MHI M state of the MHI device
 * @d_state: EP-PCIe D state of the MHI device
 */
struct mhi_ep_sm {
	struct mhi_ep_cntrl *mhi_cntrl;
	struct mutex lock;
	struct workqueue_struct *sm_wq;
	struct work_struct sm_work;
	enum mhi_ep_state state;
	enum mhi_ep_pcie_state d_state;
};

struct mhi_ep_chan {
	char *name;
	u32 chan;
	struct mhi_ep_ring ring;
	struct mhi_ep_device *mhi_dev;
	enum mhi_ep_ch_ctx_state state;
	enum dma_data_direction dir;
	struct mutex lock;

	/* Channel specific callbacks */
	void (*xfer_cb)(struct mhi_ep_device *mhi_dev, struct mhi_result *result);

	bool configured;
	bool skip_td;
	/* current TRE being processed */
	uint64_t			tre_loc;
	/* current TRE size */
	uint32_t			tre_size;
	/* tre bytes left to read/write */
	uint32_t			tre_bytes_left;

	/* TODO */
	void __iomem *tre_buf;
	phys_addr_t tre_phys;
};

/* MHI Ring related functions */

int mhi_ep_process_cmd_ring(struct mhi_ep_ring *ring, union mhi_ep_ring_element_type *el);
int mhi_ep_process_tre_ring(struct mhi_ep_ring *ring, union mhi_ep_ring_element_type *el);
void mhi_ep_ring_init(struct mhi_ep_ring *ring, enum mhi_ep_ring_type type, u32 id);
/**
 * mhi_ep_ring_start() - Fetches the respective transfer ring's context from
 *		the host and updates the write offset.
 * @ring:	Ring for the respective context - Channel/Event/Command.
 * @ctx:	Transfer ring of type mhi_ep_ring_ctx.
 */
int mhi_ep_ring_start(struct mhi_ep_cntrl *mhi_cntrl, struct mhi_ep_ring *ring, union mhi_ep_ring_ctx *ctx);

/**
 * mhi_ep_update_wr_offset() - Check for any updates in the write offset.
 * @ring:	Ring for the respective context - Channel/Event/Command.
 */
int mhi_ep_update_wr_offset(struct mhi_ep_ring *ring);

/**
 * mhi_ep_process_ring() - Update the Write pointer, fetch the ring elements
 *			    and invoke the clients callback.
 * @ring:	Ring for the respective context - Channel/Event/Command.
 */
int mhi_ep_process_ring(struct mhi_ep_ring *ring);

/**
 * mhi_ep_process_ring_element() - Fetch the ring elements and invoke the
 *			    clients callback.
 * @ring:	Ring for the respective context - Channel/Event/Command.
 * @offset:	Offset index into the respective ring's cache element.
 */
int mhi_ep_process_ring_element(struct mhi_ep_ring *ring, size_t offset);

/**
 * mhi_ep_ring_add_element() - Copy the element to the respective transfer rings
 *			read pointer and increment the index.
 * @ring:	Ring for the respective context - Channel/Event/Command.
 * @element:	Transfer ring element to be copied to the host memory.
 */
int mhi_ep_ring_add_element(struct mhi_ep_cntrl *mhi_cntrl, struct mhi_ep_ring *ring,
				union mhi_ep_ring_element_type *element,
				struct event_req *ereq, int evt_offset);

/* MMIO related functions */

/**
 * mhi_ep_mmio_read() - Generic MHI MMIO register read API.
 * @mhi:	MHI device structure.
 * @offset:	MHI address offset from base.
 * @regval:	Pointer the register value is stored to.
 */
void mhi_ep_mmio_read(struct mhi_ep_cntrl *mhi_cntrl, u32 offset, u32 *regval);

/**
 * mhi_ep_mmio_read() - Generic MHI MMIO register write API.
 * @mhi:	MHI device structure.
 * @offset:	MHI address offset from base.
 * @val:	Value to be written to the register offset.
 */
void mhi_ep_mmio_write(struct mhi_ep_cntrl *mhi_cntrl, u32 offset, u32 val);

/**
 * mhi_ep_mmio_masked_write() - Generic MHI MMIO register write masked API.
 * @mhi:	MHI device structure.
 * @offset:	MHI address offset from base.
 * @mask:	Register field mask.
 * @shift:	Shift value
 * @val:	Value to be written to the register offset.
 */
void mhi_ep_mmio_masked_write(struct mhi_ep_cntrl *mhi_cntrl, u32 offset,
			       u32 mask, u32 shift, u32 val);

/**
 * mhi_ep_mmio_masked_read() - Generic MHI MMIO register read masked API.
 * @dev:	MHI device structure.
 * @offset:	MHI address offset from base.
 * @mask:	Register field mask.
 * @shift:	Register field mask shift value.
 * @regval:	Pointer the register value is stored to.
 */
int mhi_ep_mmio_masked_read(struct mhi_ep_cntrl *dev, u32 offset,
			     u32 mask, u32 shift, u32 *regval);

/**
 * mhi_ep_mmio_enable_ctrl_interrupt() - Enable Control interrupt.
 * @mhi:	MHI device structure.
 */

void mhi_ep_mmio_enable_ctrl_interrupt(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_disable_ctrl_interrupt() - Disable Control interrupt.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_disable_ctrl_interrupt(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_read_ctrl_status_interrupt() - Read Control interrupt status.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_read_ctrl_status_interrupt(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_enable_cmdb_interrupt() - Enable Command doorbell interrupt.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_enable_cmdb_interrupt(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_disable_cmdb_interrupt() - Disable Command doorbell interrupt.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_disable_cmdb_interrupt(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_read_cmdb_interrupt() - Read Command doorbell status.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_read_cmdb_status_interrupt(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_enable_chdb_a7() - Enable Channel doorbell for a given
 *		channel id.
 * @mhi:	MHI device structure.
 * @chdb_id:	Channel id number.
 */
void mhi_ep_mmio_enable_chdb_a7(struct mhi_ep_cntrl *mhi_cntrl, u32 chdb_id);
/**
 * mhi_ep_mmio_disable_chdb_a7() - Disable Channel doorbell for a given
 *		channel id.
 * @mhi:	MHI device structure.
 * @chdb_id:	Channel id number.
 */
void mhi_ep_mmio_disable_chdb_a7(struct mhi_ep_cntrl *mhi_cntrl, u32 chdb_id);

/**
 * mhi_ep_mmio_enable_erdb_a7() - Enable Event ring doorbell for a given
 *		event ring id.
 * @mhi:	MHI device structure.
 * @erdb_id:	Event ring id number.
 */
void mhi_ep_mmio_enable_erdb_a7(struct mhi_ep_cntrl *mhi_cntrl, u32 erdb_id);

/**
 * mhi_ep_mmio_disable_erdb_a7() - Disable Event ring doorbell for a given
 *		event ring id.
 * @mhi:	MHI device structure.
 * @erdb_id:	Event ring id number.
 */
void mhi_ep_mmio_disable_erdb_a7(struct mhi_ep_cntrl *mhi_cntrl, u32 erdb_id);

/**
 * mhi_ep_mmio_enable_chdb_interrupts() - Enable all Channel doorbell
 *		interrupts.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_enable_chdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_mask_chdb_interrupts() - Mask all Channel doorbell
 *		interrupts.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_mask_chdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_read_chdb_interrupts() - Read all Channel doorbell
 *		interrupts.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_read_chdb_status_interrupts(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_enable_erdb_interrupts() - Enable all Event doorbell
 *		interrupts.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_enable_erdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl);

/**
 *mhi_ep_mmio_mask_erdb_interrupts() - Mask all Event doorbell
 *		interrupts.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_mask_erdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_read_erdb_interrupts() - Read all Event doorbell
 *		interrupts.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_read_erdb_status_interrupts(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_mask_interrupts() - Mask all MHI interrupts.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_mask_interrupts(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_clear_interrupts() - Clear all doorbell interrupts.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_clear_interrupts(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_get_chc_base() - Fetch the Channel ring context base address.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_get_chc_base(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_get_erc_base() - Fetch the Event ring context base address.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_get_erc_base(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_get_crc_base() - Fetch the Command ring context base address.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_get_crc_base(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_get_ch_db() - Fetch the Write offset of the Channel ring ID.
 * @mhi:	MHI device structure.
 * @wr_offset:	Pointer of the write offset to be written to.
 */
void mhi_ep_mmio_get_ch_db(struct mhi_ep_ring *ring, u64 *wr_offset);

/**
 * mhi_ep_get_er_db() - Fetch the Write offset of the Event ring ID.
 * @mhi:	MHI device structure.
 * @wr_offset:	Pointer of the write offset to be written to.
 */
void mhi_ep_mmio_get_er_db(struct mhi_ep_ring *ring, u64 *wr_offset);

/**
 * mhi_ep_get_cmd_base() - Fetch the Write offset of the Command ring ID.
 * @mhi:	MHI device structure.
 * @wr_offset:	Pointer of the write offset to be written to.
 */
void mhi_ep_mmio_get_cmd_db(struct mhi_ep_ring *ring, u64 *wr_offset);

/**
 * mhi_ep_mmio_set_env() - Write the Execution Enviornment.
 * @mhi:	MHI device structure.
 * @value:	Value of the EXEC EVN.
 */
void mhi_ep_mmio_set_env(struct mhi_ep_cntrl *mhi_cntrl, u32 value);

/**
 * mhi_ep_mmio_clear_reset() - Clear the reset bit
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_clear_reset(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_reset() - Reset the MMIO done as part of initialization.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_reset(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_get_mhi_addr() - Fetches the Data and Control region from the Host.
 * @mhi:	MHI device structure.
 */
void mhi_ep_get_mhi_addr(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_get_mhi_state() - Fetches the MHI state such as M0/M1/M2/M3.
 * @mhi:	MHI device structure.
 * @state:	Pointer of type mhi_ep_state
 * @mhi_reset:	MHI device reset from host.
 */
void mhi_ep_mmio_get_mhi_state(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_ep_state *state,
				bool *mhi_reset);

/**
 * mhi_ep_mmio_init() - Initializes the MMIO and reads the Number of event
 *		rings, support number of channels, and offsets to the Channel
 *		and Event doorbell from the host.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_init(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_mmio_update_ner() - Update the number of event rings (NER) programmed by
 *		the host.
 * @mhi:	MHI device structure.
 */
void mhi_ep_mmio_update_ner(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_restore_mmio() - Restores the MMIO when MHI device comes out of M3.
 * @mhi:	MHI device structure.
 */
void mhi_ep_restore_mmio(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_backup_mmio() - Backup MMIO before a MHI transition to M3.
 * @mhi:	MHI device structure.
 */
void mhi_ep_backup_mmio(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_dump_mmio() - Memory dump of the MMIO region for debug.
 * @mhi:	MHI device structure.
 */
void mhi_ep_dump_mmio(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_send_state_change_event() - Send state change event to the host
 *		such as M0/M1/M2/M3.
 * @mhi:	MHI device structure.
 * @state:	MHI state of type mhi_ep_state
 */
int mhi_ep_send_state_change_event(struct mhi_ep_cntrl *mhi_cntrl,
					enum mhi_ep_state state);
/**
 * mhi_ep_send_ee_event() - Send Execution enviornment state change
 *		event to the host.
 * @mhi:	MHI device structure.
 * @state:	MHI state of type mhi_ep_execenv
 */
int mhi_ep_send_ee_event(struct mhi_ep_cntrl *mhi_cntrl,
					enum mhi_ep_execenv exec_env);
/**
 * mhi_ep_syserr() - System error when unexpected events are received.
 * @mhi:	MHI device structure.
 */
int mhi_ep_syserr(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_suspend() - MHI device suspend to stop channel processing at the
 *		Transfer ring boundary, update the channel state to suspended.
 * @mhi:	MHI device structure.
 */
int mhi_ep_suspend(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_resume() - MHI device resume to update the channel state to running.
 * @mhi:	MHI device structure.
 */
int mhi_ep_resume(struct mhi_ep_cntrl *mhi_cntrl);

/**
 * mhi_ep_trigger_hw_acc_wakeup() - Notify State machine there is HW
 *		accelerated data to be send and prevent MHI suspend.
 * @mhi:	MHI device structure.
 */
int mhi_ep_trigger_hw_acc_wakeup(struct mhi_ep_cntrl *mhi_cntrl);

int mhi_ep_sm_init(struct mhi_ep_cntrl *mhi_cntrl);
int mhi_ep_sm_exit(struct mhi_ep_cntrl *mhi_cntrl);
int mhi_ep_sm_set_ready(struct mhi_ep_cntrl *mhi_cntrl);
int mhi_ep_notify_sm_event(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_ep_event_type event);
int mhi_ep_sm_get_mhi_state(enum mhi_ep_state *state);

#endif

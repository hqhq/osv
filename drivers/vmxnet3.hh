/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef VMWARE3_DRIVER_H
#define VMWARE3_DRIVER_H

#include <bsd/porting/netport.h>
#include <bsd/porting/uma_stub.h>
#include <bsd/sys/net/if_var.h>
#include <bsd/sys/net/if.h>
#include <bsd/sys/sys/mbuf.h>

#include "drivers/driver.hh"
#include "drivers/vmxnet3-queues.hh"
#include "drivers/pci-device.hh"
#include <osv/mempool.hh>
#include <osv/interrupt.hh>
#include <osv/percpu_xmit.hh>

namespace vmw {

//PCI configurations
enum pciconf {
    vendor_id = 0x15AD,
    device_id = 0x07B0
};

enum align {
    //Shared memory alignment
    driver_shared = 1,
    queues_shared = 128,
    multicast = 32,
    //Queue descriptors alignment
    desc = 512
};

//BAR0 registers
enum bar0 {
    txh = 0x600, // Queue 0 of Tx head
    rxh1 = 0x800, // Queue 0 of Ring1 Rx head
    rxh2 = 0xA00 // Queue 0 of Ring2 Rx head
};

//BAR1 registers
enum bar1 {
    vrrs = 0x000,    // Revision
    uvrs = 0x008,    // UPT version
    dsl  = 0x010,    // Driver shared address low
    dsh  = 0x018,    // Driver shared address high
    cmd  = 0x020     // Command
};

//VMXNET3 commands
enum command {
    enable = 0xCAFE0000, // Enable VMXNET3
    disable = 0xCAFE0001, // Disable VMXNET3
    reset = 0xCAFE0002, // Reset device
    set_rxmode = 0xCAFE0003, // Set interface flags
    set_filter = 0xCAFE0004, // Set address filter
    vlan_filter = 0xCAFE0005, // Set VLAN filter
    get_status = 0xF00D0000,  // Get queue errors
    get_stats = 0xF00D0001, // Get queue statistics
    get_link = 0xF00D0002, // Get link status
    get_macl = 0xF00D0003, // Get MAC address low
    get_mach = 0xF00D0004, // Get MAC address high
    get_intrcfg = 0xF00D0008  // Get interrupt config
};

//Offloading modes
enum om {
    none = 0,
    csum = 2,
    tso = 3
};

//RX modes
enum rxmode {
    ucast = 0x01,
    mcast = 0x02,
    bcast = 0x04,
    allmulti = 0x08,
    promisc = 0x10
};

//Hardware features
enum upt1 {
    fcsum = 0x0001,
    frss = 0x0002,
    fvlan = 0x0004,
    flro = 0x0008
};

// Buffer types
enum btype {
    head = 0, // Head only
    body = 1 // Body only
};

constexpr int tx_queues = 1;
constexpr int rx_queues = 1;
constexpr int eth_alen = 1;
constexpr int multicast_max = 32;
constexpr int max_rx_segs = 17;
constexpr int num_intrs = 3;
constexpr int init_gen = 1;

static inline constexpr u32 bar0_imask(int irq)
{
    return 0x000 + irq * 8;
}

struct vmxnet3_req {
    struct mbuf *mb;
    unsigned count;
    int start;
};

/**
 * Initialize an array of containers with specific virtual address.
 * Takes Preallocated buffer address and splits it into chunks of required size,
 * associates each chunk with an array element.
 * @param va preallocated buffer address
 * @param holder array of containers
 */
template<class T> void slice_memory(void *&va, T &holder)
{
    for (auto &e : holder) {
        e.attach(va);
        va += e.size();
    }
}

template<class DescT, int NDesc>
    class vmxnet3_ring {
    public:
        vmxnet3_ring()
            : _desc_mem(DescT::size() * NDesc, align::desc)
        {
            void *va = _desc_mem.get_va();
            slice_memory(va, _desc);
        }

        unsigned head, next, fill;
        int gen;
        mmu::phys get_desc_pa() const { return _desc_mem.get_pa(); }
        static u32 get_desc_num() { return NDesc; }
        DescT* get_desc(int i) { return &_desc[i]; }
        void clear_desc(int i) { _desc[i].clear(); }
        void clear_descs() { for (int i = 0; i < NDesc; i++) clear_desc(i); }
        void increment_fill();
    private:
        memory::phys_contiguous_memory _desc_mem;
        DescT      _desc[NDesc];
    };

class vmxnet3_txqueue : public vmxnet3_txq_shared {
public:
    void init();
    void set_intr_idx(unsigned idx) { layout->intr_idx = static_cast<u8>(idx); }
    typedef vmxnet3_ring<vmxnet3_tx_desc, VMXNET3_MAX_TX_NDESC> cmdRingT;
    typedef vmxnet3_ring<vmxnet3_tx_compdesc, VMXNET3_MAX_TX_NCOMPDESC> compRingT;
    cmdRingT cmd_ring;

    compRingT comp_ring;
    struct mbuf *buf[VMXNET3_MAX_TX_NDESC];
    unsigned avail = VMXNET3_MAX_TX_NDESC;
};

class vmxnet3_rxqueue : public vmxnet3_rxq_shared {
public:
    void init();
    void set_intr_idx(unsigned idx) { layout->intr_idx = static_cast<u8>(idx); }
    void discard(int rid, int idx);
    void newbuf(int rid);

    typedef vmxnet3_ring<vmxnet3_rx_desc, VMXNET3_MAX_RX_NDESC> cmdRingT;
    typedef vmxnet3_ring<vmxnet3_rx_compdesc, VMXNET3_MAX_RX_NCOMPDESC> compRingT;

    cmdRingT cmd_rings[VMXNET3_RXRINGS_PERQ];
    compRingT comp_ring;
    struct mbuf *buf[VMXNET3_RXRINGS_PERQ][VMXNET3_MAX_RX_NDESC];

    struct mbuf *m_currpkt_head = nullptr;
    struct mbuf *m_currpkt_tail = nullptr;
};

class vmxnet3 : public hw_driver {
public:
    explicit vmxnet3(pci::device& dev);
    virtual ~vmxnet3() {};

    virtual std::string get_name() const { return "vmxnet3"; }

    virtual void dump_config(void);
    int transmit(struct mbuf* m_head);
    void receive_work();
    int xmit_prep(mbuf* m_head, void*& cooky);
    void kick_pending();
    void kick_pending_with_thresh();
    bool kick_hw();
    void wake_worker();
    int try_xmit_one_locked(void* cooky);
    void xmit_one_locked(void *req);

    static hw_driver* probe(hw_device* dev);

    /**
     * Fill the if_data buffer with data from our iface including those that
     * we have gathered by ourselvs (e.g. FP queue stats).
     * @param out_data output buffer
     */
    void fill_stats(struct if_data* out_data) const;

private:
    void parse_pci_config();
    void stop();
    void enable_device();
    void do_version_handshake();
    void attach_queues_shared();
    void fill_driver_shared();
    void allocate_interrupts();

    void set_intr_idx(unsigned idx) { _drv_shared.set_evt_intr_idx(static_cast<u8>(idx)); }
    virtual void isr() {};

    void write_cmd(u32 cmd);
    u32 read_cmd(u32 cmd);

    void get_mac_address(u_int8_t *macaddr);
    void txq_encap(vmxnet3_txqueue &txq, vmxnet3_req *req);
    int txq_offload(vmxnet3_req *req);
    void txq_gc(vmxnet3_txqueue &txq);
    void rxq_eof(vmxnet3_rxqueue &rxq);
    bool rxq_avail(vmxnet3_rxqueue &rxq);
    void rxq_input(vmxnet3_rxqueue &rxq, vmxnet3_rx_compdesc *rxcd,
        struct mbuf *m);
    void rx_csum(vmxnet3_rx_compdesc *rxcd, struct mbuf *m);
    void enable_interrupts();
    void enable_interrupt(unsigned idx);
    void disable_interrupts();
    void disable_interrupt(unsigned idx);
    int try_xmit_one_locked(vmxnet3_req *req);

    //maintains the vmxnet3 instance number for multiple adapters
    static int _instance;
    int _id;
    struct ifnet* _ifn;

    pci::device& _dev;
    interrupt_manager _msi;

    struct rxq_stats {
        u64 rx_packets; /* if_ipackets */
        u64 rx_bytes;   /* if_ibytes */
        u64 rx_drops;   /* if_iqdrops */
        u64 rx_csum;    /* number of packets with correct csum */
        u64 rx_csum_err;/* number of packets with a bad checksum */
    } _rxq_stats = { 0 };

    struct txq_stats {
        u64 tx_packets; /* if_opackets */
        u64 tx_bytes;   /* if_obytes */
        u64 tx_err;     /* Number of broken packets */
        u64 tx_drops;   /* Number of dropped packets */
        u64 tx_csum;    /* CSUM offload requests */
        u64 tx_tso;     /* GSO/TSO packets */
        /* u64 tx_rescheduled; */ /* TODO when we implement xoff */
    } _txq_stats = { 0 };

    //Shared memory
    pci::bar *_bar0 = nullptr;
    pci::bar *_bar1 = nullptr;

    memory::phys_contiguous_memory _drv_shared_mem;
    vmxnet3_drv_shared _drv_shared;

    memory::phys_contiguous_memory _queues_shared_mem;

    vmxnet3_txqueue _txq[tx_queues];
    vmxnet3_rxqueue _rxq[rx_queues];

    memory::phys_contiguous_memory _mcast_list;

    sched::thread _receive_task;

    osv::tx_xmit_iterator<vmxnet3> _xmit_it;
    osv::xmitter<vmxnet3, 4096> _xmitter;
    sched::thread _worker;

    uma_zone_t _zone_req;
};

}

#endif

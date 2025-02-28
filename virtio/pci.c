#include "kvm/virtio-pci.h"

#include "kvm/ioport.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/irq.h"
#include "kvm/virtio.h"
#include "kvm/ioeventfd.h"
#include "kvm/util.h"

#include <sys/ioctl.h>
#include <linux/virtio_pci.h>
#include <linux/byteorder.h>
#include <assert.h>
#include <string.h>

#define ALIGN_UP(x, s)		ALIGN((x) + (s) - 1, (s))
#define VIRTIO_NR_MSIX		(VIRTIO_PCI_MAX_VQ + VIRTIO_PCI_MAX_CONFIG)
#define VIRTIO_MSIX_TABLE_SIZE	(VIRTIO_NR_MSIX * 16)
#define VIRTIO_MSIX_PBA_SIZE	(ALIGN_UP(VIRTIO_MSIX_TABLE_SIZE, 64) / 8)
#define VIRTIO_MSIX_BAR_SIZE	(1UL << fls_long(VIRTIO_MSIX_TABLE_SIZE + \
						 VIRTIO_MSIX_PBA_SIZE))

static u16 virtio_pci__port_addr(struct virtio_pci *vpci)
{
	return pci__bar_address(&vpci->pci_hdr, 0);
}

static u32 virtio_pci__mmio_addr(struct virtio_pci *vpci)
{
	return pci__bar_address(&vpci->pci_hdr, 1);
}

static u32 virtio_pci__msix_io_addr(struct virtio_pci *vpci)
{
	return pci__bar_address(&vpci->pci_hdr, 2);
}

static int virtio_pci__add_msix_route(struct virtio_pci *vpci, u32 vec)
{
	int gsi;
	struct msi_msg *msg;

	if (vec == VIRTIO_MSI_NO_VECTOR)
		return -EINVAL;

	msg = &vpci->msix_table[vec].msg;
	gsi = irq__add_msix_route(vpci->kvm, msg, vpci->dev_hdr.dev_num << 3);
	/*
	 * We don't need IRQ routing if we can use
	 * MSI injection via the KVM_SIGNAL_MSI ioctl.
	 */
	if (gsi == -ENXIO && vpci->features & VIRTIO_PCI_F_SIGNAL_MSI)
		return gsi;

	if (gsi < 0)
		die("failed to configure MSIs");

	return gsi;
}

static void virtio_pci__ioevent_callback(struct kvm *kvm, void *param)
{
	struct virtio_pci_ioevent_param *ioeventfd = param;
	struct virtio_pci *vpci = ioeventfd->vdev->virtio;

	ioeventfd->vdev->ops->notify_vq(kvm, vpci->dev, ioeventfd->vq);
}

static int virtio_pci__init_ioeventfd(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct ioevent ioevent;
	struct virtio_pci *vpci = vdev->virtio;
	u32 mmio_addr = virtio_pci__mmio_addr(vpci);
	u16 port_addr = virtio_pci__port_addr(vpci);
	int r, flags = 0;
	int fd;

	vpci->ioeventfds[vq] = (struct virtio_pci_ioevent_param) {
		.vdev		= vdev,
		.vq		= vq,
	};

	ioevent = (struct ioevent) {
		.fn		= virtio_pci__ioevent_callback,
		.fn_ptr		= &vpci->ioeventfds[vq],
		.datamatch	= vq,
		.fn_kvm		= kvm,
	};

	/*
	 * Vhost will poll the eventfd in host kernel side, otherwise we
	 * need to poll in userspace.
	 */
	if (!vdev->use_vhost)
		flags |= IOEVENTFD_FLAG_USER_POLL;

	/* ioport */
	ioevent.io_addr	= port_addr + VIRTIO_PCI_QUEUE_NOTIFY;
	ioevent.io_len	= sizeof(u16);
	ioevent.fd	= fd = eventfd(0, 0);
	r = ioeventfd__add_event(&ioevent, flags | IOEVENTFD_FLAG_PIO);
	if (r)
		return r;

	/* mmio */
	ioevent.io_addr	= mmio_addr + VIRTIO_PCI_QUEUE_NOTIFY;
	ioevent.io_len	= sizeof(u16);
	ioevent.fd	= eventfd(0, 0);
	r = ioeventfd__add_event(&ioevent, flags);
	if (r)
		goto free_ioport_evt;

	if (vdev->ops->notify_vq_eventfd)
		vdev->ops->notify_vq_eventfd(kvm, vpci->dev, vq, fd);
	return 0;

free_ioport_evt:
	ioeventfd__del_event(port_addr + VIRTIO_PCI_QUEUE_NOTIFY, vq);
	return r;
}

static void virtio_pci_exit_vq(struct kvm *kvm, struct virtio_device *vdev,
			       int vq)
{
	struct virtio_pci *vpci = vdev->virtio;
	u32 mmio_addr = virtio_pci__mmio_addr(vpci);
	u16 port_addr = virtio_pci__port_addr(vpci);

	ioeventfd__del_event(mmio_addr + VIRTIO_PCI_QUEUE_NOTIFY, vq);
	ioeventfd__del_event(port_addr + VIRTIO_PCI_QUEUE_NOTIFY, vq);
	virtio_exit_vq(kvm, vdev, vpci->dev, vq);
}

static inline bool virtio_pci__msix_enabled(struct virtio_pci *vpci)
{
	return vpci->pci_hdr.msix.ctrl & cpu_to_le16(PCI_MSIX_FLAGS_ENABLE);
}

static bool virtio_pci__specific_data_in(struct kvm *kvm, struct virtio_device *vdev,
					 void *data, u32 size, unsigned long offset)
{
	u32 config_offset;
	struct virtio_pci *vpci = vdev->virtio;
	int type = virtio__get_dev_specific_field(offset - 20,
							virtio_pci__msix_enabled(vpci),
							&config_offset);
	if (type == VIRTIO_PCI_O_MSIX) {
		switch (offset) {
		case VIRTIO_MSI_CONFIG_VECTOR:
			ioport__write16(data, vpci->config_vector);
			break;
		case VIRTIO_MSI_QUEUE_VECTOR:
			ioport__write16(data, vpci->vq_vector[vpci->queue_selector]);
			break;
		};

		return true;
	} else if (type == VIRTIO_PCI_O_CONFIG) {
		return virtio_access_config(kvm, vdev, vpci->dev, config_offset,
					    data, size, false);
	}

	return false;
}

static bool virtio_pci__data_in(struct kvm_cpu *vcpu, struct virtio_device *vdev,
				unsigned long offset, void *data, u32 size)
{
	bool ret = true;
	struct virtio_pci *vpci;
	struct virt_queue *vq;
	struct kvm *kvm;
	u32 val;

	kvm = vcpu->kvm;
	vpci = vdev->virtio;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		val = vdev->ops->get_host_features(kvm, vpci->dev);
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		vq = vdev->ops->get_vq(kvm, vpci->dev, vpci->queue_selector);
		ioport__write32(data, vq->vring_addr.pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		val = vdev->ops->get_size_vq(kvm, vpci->dev, vpci->queue_selector);
		ioport__write16(data, val);
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, vpci->status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, vpci->isr);
		kvm__irq_line(kvm, vpci->legacy_irq_line, VIRTIO_IRQ_LOW);
		vpci->isr = VIRTIO_IRQ_LOW;
		break;
	default:
		ret = virtio_pci__specific_data_in(kvm, vdev, data, size, offset);
		break;
	};

	return ret;
}

static void update_msix_map(struct virtio_pci *vpci,
			    struct msix_table *msix_entry, u32 vecnum)
{
	u32 gsi, i;

	/* Find the GSI number used for that vector */
	if (vecnum == vpci->config_vector) {
		gsi = vpci->config_gsi;
	} else {
		for (i = 0; i < VIRTIO_PCI_MAX_VQ; i++)
			if (vpci->vq_vector[i] == vecnum)
				break;
		if (i == VIRTIO_PCI_MAX_VQ)
			return;
		gsi = vpci->gsis[i];
	}

	if (gsi == 0)
		return;

	msix_entry = &msix_entry[vecnum];
	irq__update_msix_route(vpci->kvm, gsi, &msix_entry->msg);
}

static bool virtio_pci__specific_data_out(struct kvm *kvm, struct virtio_device *vdev,
					  void *data, u32 size, unsigned long offset)
{
	struct virtio_pci *vpci = vdev->virtio;
	u32 config_offset, vec;
	int gsi;
	int type = virtio__get_dev_specific_field(offset - 20, virtio_pci__msix_enabled(vpci),
							&config_offset);
	if (type == VIRTIO_PCI_O_MSIX) {
		switch (offset) {
		case VIRTIO_MSI_CONFIG_VECTOR:
			vec = vpci->config_vector = ioport__read16(data);

			gsi = virtio_pci__add_msix_route(vpci, vec);
			if (gsi < 0)
				break;

			vpci->config_gsi = gsi;
			break;
		case VIRTIO_MSI_QUEUE_VECTOR:
			vec = ioport__read16(data);
			vpci->vq_vector[vpci->queue_selector] = vec;

			gsi = virtio_pci__add_msix_route(vpci, vec);
			if (gsi < 0)
				break;

			vpci->gsis[vpci->queue_selector] = gsi;
			if (vdev->ops->notify_vq_gsi)
				vdev->ops->notify_vq_gsi(kvm, vpci->dev,
							 vpci->queue_selector,
							 gsi);
			break;
		};

		return true;
	} else if (type == VIRTIO_PCI_O_CONFIG) {
		return virtio_access_config(kvm, vdev, vpci->dev, config_offset,
					    data, size, true);
	}

	return false;
}

static bool virtio_pci__data_out(struct kvm_cpu *vcpu, struct virtio_device *vdev,
				 unsigned long offset, void *data, u32 size)
{
	bool ret = true;
	struct virtio_pci *vpci;
	struct virt_queue *vq;
	struct kvm *kvm;
	u32 val;
	unsigned int vq_count;

	kvm = vcpu->kvm;
	vpci = vdev->virtio;
	vq_count = vdev->ops->get_vq_count(kvm, vpci->dev);

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		val = ioport__read32(data);
		virtio_set_guest_features(kvm, vdev, vpci->dev, val);
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		val = ioport__read32(data);
		if (val) {
			vq = vdev->ops->get_vq(kvm, vpci->dev,
					       vpci->queue_selector);
			vq->vring_addr = (struct vring_addr) {
				.legacy	= true,
				.pfn	= val,
				.align	= VIRTIO_PCI_VRING_ALIGN,
				.pgsize	= 1 << VIRTIO_PCI_QUEUE_ADDR_SHIFT,
			};
			virtio_pci__init_ioeventfd(kvm, vdev,
						   vpci->queue_selector);
			vdev->ops->init_vq(kvm, vpci->dev,
					   vpci->queue_selector);
		} else {
			virtio_pci_exit_vq(kvm, vdev, vpci->queue_selector);
		}
		break;
	case VIRTIO_PCI_QUEUE_SEL:
		val = ioport__read16(data);
		if (val >= vq_count) {
			WARN_ONCE(1, "QUEUE_SEL value (%u) is larger than VQ count (%u)\n",
				val, vq_count);
			return false;
		}
		vpci->queue_selector = val;
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY:
		val = ioport__read16(data);
		if (val >= vq_count) {
			WARN_ONCE(1, "QUEUE_SEL value (%u) is larger than VQ count (%u)\n",
				val, vq_count);
			return false;
		}
		vdev->ops->notify_vq(kvm, vpci->dev, val);
		break;
	case VIRTIO_PCI_STATUS:
		vpci->status = ioport__read8(data);
		if (!vpci->status) /* Sample endianness on reset */
			vdev->endian = kvm_cpu__get_endianness(vcpu);
		virtio_notify_status(kvm, vdev, vpci->dev, vpci->status);
		break;
	default:
		ret = virtio_pci__specific_data_out(kvm, vdev, data, size, offset);
		break;
	};

	return ret;
}

static void virtio_pci__msix_mmio_callback(struct kvm_cpu *vcpu,
					   u64 addr, u8 *data, u32 len,
					   u8 is_write, void *ptr)
{
	struct virtio_device *vdev = ptr;
	struct virtio_pci *vpci = vdev->virtio;
	struct msix_table *table;
	u32 msix_io_addr = virtio_pci__msix_io_addr(vpci);
	u32 pba_offset;
	int vecnum;
	size_t offset;

	BUILD_BUG_ON(VIRTIO_NR_MSIX > (sizeof(vpci->msix_pba) * 8));

	pba_offset = vpci->pci_hdr.msix.pba_offset & ~PCI_MSIX_TABLE_BIR;
	if (addr >= msix_io_addr + pba_offset) {
		/* Read access to PBA */
		if (is_write)
			return;
		offset = addr - (msix_io_addr + pba_offset);
		if ((offset + len) > sizeof (vpci->msix_pba))
			return;
		memcpy(data, (void *)&vpci->msix_pba + offset, len);
		return;
	}

	table  = vpci->msix_table;
	offset = addr - msix_io_addr;

	vecnum = offset / sizeof(struct msix_table);
	offset = offset % sizeof(struct msix_table);

	if (!is_write) {
		memcpy(data, (void *)&table[vecnum] + offset, len);
		return;
	}

	memcpy((void *)&table[vecnum] + offset, data, len);

	/* Did we just update the address or payload? */
	if (offset < offsetof(struct msix_table, ctrl))
		update_msix_map(vpci, table, vecnum);
}

static void virtio_pci__signal_msi(struct kvm *kvm, struct virtio_pci *vpci,
				   int vec)
{
	struct kvm_msi msi = {
		.address_lo = vpci->msix_table[vec].msg.address_lo,
		.address_hi = vpci->msix_table[vec].msg.address_hi,
		.data = vpci->msix_table[vec].msg.data,
	};

	if (kvm->msix_needs_devid) {
		msi.flags = KVM_MSI_VALID_DEVID;
		msi.devid = vpci->dev_hdr.dev_num << 3;
	}

	irq__signal_msi(kvm, &msi);
}

int virtio_pci__signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct virtio_pci *vpci = vdev->virtio;
	int tbl = vpci->vq_vector[vq];

	if (virtio_pci__msix_enabled(vpci) && tbl != VIRTIO_MSI_NO_VECTOR) {
		if (vpci->pci_hdr.msix.ctrl & cpu_to_le16(PCI_MSIX_FLAGS_MASKALL) ||
		    vpci->msix_table[tbl].ctrl & cpu_to_le16(PCI_MSIX_ENTRY_CTRL_MASKBIT)) {

			vpci->msix_pba |= 1 << tbl;
			return 0;
		}

		if (vpci->features & VIRTIO_PCI_F_SIGNAL_MSI)
			virtio_pci__signal_msi(kvm, vpci, vpci->vq_vector[vq]);
		else
			kvm__irq_trigger(kvm, vpci->gsis[vq]);
	} else {
		vpci->isr = VIRTIO_IRQ_HIGH;
		kvm__irq_line(kvm, vpci->legacy_irq_line, VIRTIO_IRQ_HIGH);
	}
	return 0;
}

int virtio_pci__signal_config(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_pci *vpci = vdev->virtio;
	int tbl = vpci->config_vector;

	if (virtio_pci__msix_enabled(vpci) && tbl != VIRTIO_MSI_NO_VECTOR) {
		if (vpci->pci_hdr.msix.ctrl & cpu_to_le16(PCI_MSIX_FLAGS_MASKALL) ||
		    vpci->msix_table[tbl].ctrl & cpu_to_le16(PCI_MSIX_ENTRY_CTRL_MASKBIT)) {

			vpci->msix_pba |= 1 << tbl;
			return 0;
		}

		if (vpci->features & VIRTIO_PCI_F_SIGNAL_MSI)
			virtio_pci__signal_msi(kvm, vpci, tbl);
		else
			kvm__irq_trigger(kvm, vpci->config_gsi);
	} else {
		vpci->isr = VIRTIO_PCI_ISR_CONFIG;
		kvm__irq_trigger(kvm, vpci->legacy_irq_line);
	}

	return 0;
}

static void virtio_pci__io_mmio_callback(struct kvm_cpu *vcpu,
					 u64 addr, u8 *data, u32 len,
					 u8 is_write, void *ptr)
{
	struct virtio_device *vdev = ptr;
	struct virtio_pci *vpci = vdev->virtio;
	u32 ioport_addr = virtio_pci__port_addr(vpci);
	u32 base_addr;

	if (addr >= ioport_addr &&
	    addr < ioport_addr + pci__bar_size(&vpci->pci_hdr, 0))
		base_addr = ioport_addr;
	else
		base_addr = virtio_pci__mmio_addr(vpci);

	if (!is_write)
		virtio_pci__data_in(vcpu, vdev, addr - base_addr, data, len);
	else
		virtio_pci__data_out(vcpu, vdev, addr - base_addr, data, len);
}

static int virtio_pci__bar_activate(struct kvm *kvm,
				    struct pci_device_header *pci_hdr,
				    int bar_num, void *data)
{
	struct virtio_device *vdev = data;
	u32 bar_addr, bar_size;
	int r = -EINVAL;

	assert(bar_num <= 2);

	bar_addr = pci__bar_address(pci_hdr, bar_num);
	bar_size = pci__bar_size(pci_hdr, bar_num);

	switch (bar_num) {
	case 0:
		r = kvm__register_pio(kvm, bar_addr, bar_size,
				      virtio_pci__io_mmio_callback, vdev);
		break;
	case 1:
		r =  kvm__register_mmio(kvm, bar_addr, bar_size, false,
					virtio_pci__io_mmio_callback, vdev);
		break;
	case 2:
		r =  kvm__register_mmio(kvm, bar_addr, bar_size, false,
					virtio_pci__msix_mmio_callback, vdev);
		break;
	}

	return r;
}

static int virtio_pci__bar_deactivate(struct kvm *kvm,
				      struct pci_device_header *pci_hdr,
				      int bar_num, void *data)
{
	u32 bar_addr;
	bool success;
	int r = -EINVAL;

	assert(bar_num <= 2);

	bar_addr = pci__bar_address(pci_hdr, bar_num);

	switch (bar_num) {
	case 0:
		r = kvm__deregister_pio(kvm, bar_addr);
		break;
	case 1:
	case 2:
		success = kvm__deregister_mmio(kvm, bar_addr);
		/* kvm__deregister_mmio fails when the region is not found. */
		r = (success ? 0 : -ENOENT);
		break;
	}

	return r;
}

int virtio_pci__init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		     int device_id, int subsys_id, int class)
{
	struct virtio_pci *vpci = vdev->virtio;
	u32 mmio_addr, msix_io_block;
	u16 port_addr;
	int r;

	vpci->kvm = kvm;
	vpci->dev = dev;

	BUILD_BUG_ON(!is_power_of_two(PCI_IO_SIZE));

	port_addr = pci_get_io_port_block(PCI_IO_SIZE);
	mmio_addr = pci_get_mmio_block(PCI_IO_SIZE);
	msix_io_block = pci_get_mmio_block(VIRTIO_MSIX_BAR_SIZE);

	vpci->pci_hdr = (struct pci_device_header) {
		.vendor_id		= cpu_to_le16(PCI_VENDOR_ID_REDHAT_QUMRANET),
		.device_id		= cpu_to_le16(device_id),
		.command		= PCI_COMMAND_IO | PCI_COMMAND_MEMORY,
		.header_type		= PCI_HEADER_TYPE_NORMAL,
		.revision_id		= 0,
		.class[0]		= class & 0xff,
		.class[1]		= (class >> 8) & 0xff,
		.class[2]		= (class >> 16) & 0xff,
		.subsys_vendor_id	= cpu_to_le16(PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET),
		.subsys_id		= cpu_to_le16(subsys_id),
		.bar[0]			= cpu_to_le32(port_addr
							| PCI_BASE_ADDRESS_SPACE_IO),
		.bar[1]			= cpu_to_le32(mmio_addr
							| PCI_BASE_ADDRESS_SPACE_MEMORY),
		.bar[2]			= cpu_to_le32(msix_io_block
							| PCI_BASE_ADDRESS_SPACE_MEMORY),
		.status			= cpu_to_le16(PCI_STATUS_CAP_LIST),
		.capabilities		= (void *)&vpci->pci_hdr.msix - (void *)&vpci->pci_hdr,
		.bar_size[0]		= cpu_to_le32(PCI_IO_SIZE),
		.bar_size[1]		= cpu_to_le32(PCI_IO_SIZE),
		.bar_size[2]		= cpu_to_le32(VIRTIO_MSIX_BAR_SIZE),
	};

	r = pci__register_bar_regions(kvm, &vpci->pci_hdr,
				      virtio_pci__bar_activate,
				      virtio_pci__bar_deactivate, vdev);
	if (r < 0)
		return r;

	vpci->dev_hdr = (struct device_header) {
		.bus_type		= DEVICE_BUS_PCI,
		.data			= &vpci->pci_hdr,
	};

	vpci->pci_hdr.msix.cap = PCI_CAP_ID_MSIX;
	vpci->pci_hdr.msix.next = 0;
	/*
	 * We at most have VIRTIO_NR_MSIX entries (VIRTIO_PCI_MAX_VQ
	 * entries for virt queue, VIRTIO_PCI_MAX_CONFIG entries for
	 * config).
	 *
	 * To quote the PCI spec:
	 *
	 * System software reads this field to determine the
	 * MSI-X Table Size N, which is encoded as N-1.
	 * For example, a returned value of "00000000011"
	 * indicates a table size of 4.
	 */
	vpci->pci_hdr.msix.ctrl = cpu_to_le16(VIRTIO_NR_MSIX - 1);

	/* Both table and PBA are mapped to the same BAR (2) */
	vpci->pci_hdr.msix.table_offset = cpu_to_le32(2);
	vpci->pci_hdr.msix.pba_offset = cpu_to_le32(2 | VIRTIO_MSIX_TABLE_SIZE);
	vpci->config_vector = 0;

	if (irq__can_signal_msi(kvm))
		vpci->features |= VIRTIO_PCI_F_SIGNAL_MSI;

	vpci->legacy_irq_line = pci__assign_irq(&vpci->pci_hdr);

	r = device__register(&vpci->dev_hdr);
	if (r < 0)
		return r;

	return 0;
}

int virtio_pci__reset(struct kvm *kvm, struct virtio_device *vdev)
{
	unsigned int vq;
	struct virtio_pci *vpci = vdev->virtio;

	for (vq = 0; vq < vdev->ops->get_vq_count(kvm, vpci->dev); vq++)
		virtio_pci_exit_vq(kvm, vdev, vq);

	return 0;
}

int virtio_pci__exit(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_pci *vpci = vdev->virtio;

	virtio_pci__reset(kvm, vdev);
	kvm__deregister_mmio(kvm, virtio_pci__mmio_addr(vpci));
	kvm__deregister_mmio(kvm, virtio_pci__msix_io_addr(vpci));
	kvm__deregister_pio(kvm, virtio_pci__port_addr(vpci));

	return 0;
}

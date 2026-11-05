#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "qemu/log.h"

static uint64_t ipod_touch_mipi_dsi_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMIPIDSIState *s = (IPodTouchMIPIDSIState *)opaque;
    switch(addr)
    {
        case REG_STATUS: {
            uint32_t status = 0x3;
            status |= (s->clkctrl & rDSIM_CLKCTRL_TxRequestHsClk) ?
                      rDSIM_STATUS_TxReadyHsClk : rDSIM_STATUS_StopStateClk;
            if (s->swrst_released) {
                status |= rDSIM_STATUS_SwRstRelease;
            }
            return status;
        }
        case REG_ESCMODE:
            return s->escmode;
        case REG_INTSRC:
            return rDSIM_INTSRC_RxDatDone;
        case REG_RXFIFO:
            if(!s->return_panel_id) {
                s->return_panel_id = true;
                // TODO this should be rewritten as a proper queue!
                return DSIM_RSP_LONG_READ | (3 << 8); // the latter part indicates the length of the response (the panel ID)
            } else {
                s->return_panel_id = false;
                return 0x00a1d13c;
            }
            
        case REG_FIFOCTRL:
            return rDSIM_FIFOCTRL_EmptyHSfr;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: read invalid location 0x%08lx.\n", __func__, addr);
            break;
    }
    return 0;
}

static void ipod_touch_mipi_dsi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMIPIDSIState *s = (IPodTouchMIPIDSIState *)opaque;

    switch(addr)
    {
        case REG_PKTHDR:
            s->pkthdr_reg = val;
            break;
        case REG_CLKCTRL:
            s->clkctrl = val;
            break;
        case REG_SWRST:
            if (val & 1) {
                s->escmode = 0;
                s->swrst_released = true;
            }
            break;
        case REG_ESCMODE:
            s->escmode = val;
            break;
        default:
            break;
    }
}

static const MemoryRegionOps mipi_dsi_ops = {
    .read = ipod_touch_mipi_dsi_read,
    .write = ipod_touch_mipi_dsi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_mipi_dsi_reset(DeviceState *dev)
{
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(dev);

    s->clkctrl = 0;
    s->escmode = 0;
    s->pkthdr_reg = 0;
    s->swrst_released = false;
    s->return_panel_id = false;
}

static void ipod_touch_mipi_dsi_realize(DeviceState *dev, Error **errp)
{
    
}

static void ipod_touch_mipi_dsi_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(dev);

    memory_region_init_io(&s->iomem, obj, &mipi_dsi_ops, s, "mipi_dsi", 0x10000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->return_panel_id = 0;
}

static void ipod_touch_mipi_dsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_mipi_dsi_realize;
    dc->reset = ipod_touch_mipi_dsi_reset;
}

static const TypeInfo ipod_touch_mipi_dsi_info = {
    .name          = TYPE_IPOD_TOUCH_MIPI_DSI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMIPIDSIState),
    .instance_init = ipod_touch_mipi_dsi_init,
    .class_init    = ipod_touch_mipi_dsi_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_mipi_dsi_info);
}

type_init(ipod_touch_machine_types)

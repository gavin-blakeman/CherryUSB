/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_core"
//#include "usb_log.h"

struct usbh_class_info *usbh_class_info_table_begin = NULL;
struct usbh_class_info *usbh_class_info_table_end = NULL;

usb_slist_t g_bus_head = USB_SLIST_OBJECT_INIT(g_bus_head);

struct setup_align_buffer 
{
  uint8_t buffer[USB_ALIGN_UP(8, CONFIG_USB_ALIGN_SIZE)];
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t ep0_request_buffer[CONFIG_USBHOST_MAX_BUS][USB_ALIGN_UP(CONFIG_USBHOST_REQUEST_BUFFER_LEN, CONFIG_USB_ALIGN_SIZE)];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX struct setup_align_buffer g_setup_buffer[CONFIG_USBHOST_MAX_BUS][CONFIG_USBHOST_MAX_EXTHUBS + 1][CONFIG_USBHOST_MAX_EHPORTS];

// Global buffer for storing bus information.
struct usbh_bus g_usbhost_bus[CONFIG_USBHOST_MAX_BUS];

/* general descriptor field offsets */
#define DESC_bLength         0 /** Length offset */
#define DESC_bDescriptorType 1 /** Descriptor type offset */

#define USB_DEV_ADDR_MAX         0x7f
#define USB_DEV_ADDR_MARK_OFFSET 5
#define USB_DEV_ADDR_MARK_MASK   0x1f

static void dummy_event_handler(uint8_t busid, uint8_t hub_index, uint8_t hub_port, uint8_t intf, uint8_t event)
{
  (void)busid;
  (void)hub_index;
  (void)hub_port;
  (void)intf;
  (void)event;
}

static int usbh_allocate_devaddr(struct usbh_devaddr_map *devgen)
{
  uint8_t lastaddr = devgen->last;
  uint8_t devaddr = lastaddr;
  int index;
  int bitno;

  for (;;) 
  {
    devaddr++;
    if (devaddr > 0x7f) 
    {
      devaddr = 2;
    }
    if (devaddr == lastaddr) 
    {
      return -USB_ERR_NOMEM;
    }

    index = devaddr >> 5;
    bitno = devaddr & 0x1f;
    if ((devgen->alloctab[index] & (1ul << bitno)) == 0) 
    {
      devgen->alloctab[index] |= (1ul << bitno);
      devgen->last = devaddr;
      return (int)devaddr;
    }
  }
}

static int __usbh_free_devaddr(struct usbh_devaddr_map *devgen, uint8_t devaddr)
{
    int index;
    int bitno;

    if ((devaddr > 0) && (devaddr < USB_DEV_ADDR_MAX)) {
        index = devaddr >> USB_DEV_ADDR_MARK_OFFSET;
        bitno = devaddr & USB_DEV_ADDR_MARK_MASK;

        /* Free the address  */
        if ((devgen->alloctab[index] & (1ul << bitno)) != 0) {
            devgen->alloctab[index] &= ~(1ul << bitno);
        } else {
            return -1;
        }
    }

    return 0;
}

static int usbh_free_devaddr(struct usbh_hubport *hport)
{
    if (hport->dev_addr > 0) {
        __usbh_free_devaddr(&hport->bus->devgen, hport->dev_addr);
    }
    return 0;
}

static const struct usbh_class_driver *usbh_find_class_driver(uint8_t class, uint8_t subclass, uint8_t protocol, uint8_t intf,
                                                              uint16_t vid, uint16_t pid)
{
    struct usbh_class_info *index = NULL;

    for (index = usbh_class_info_table_begin; index < usbh_class_info_table_end; index++) {
        if ((index->match_flags & USB_CLASS_MATCH_INTF_CLASS) && !(index->bInterfaceClass == class)) {
            continue;
        }
        if ((index->match_flags & USB_CLASS_MATCH_INTF_SUBCLASS) && !(index->bInterfaceSubClass == subclass)) {
            continue;
        }
        if ((index->match_flags & USB_CLASS_MATCH_INTF_PROTOCOL) && !(index->bInterfaceProtocol == protocol)) {
            continue;
        }
        if ((index->match_flags & USB_CLASS_MATCH_INTF_NUM) && !(index->bInterfaceNumber == intf)) {
            continue;
        }
        if (index->match_flags & USB_CLASS_MATCH_VID_PID && index->id_table) {
            /* scan id table */
            uint32_t i;
            for (i = 0; index->id_table[i][0]; i++) {
                if (index->id_table[i][0] == vid && index->id_table[i][1] == pid) {
                    break;
                }
            }
            /* do not match, continue next */
            if (!index->id_table[i][0]) {
                continue;
            }
        }
        return index->class_driver;
    }
    return NULL;
}

static int parse_device_descriptor(struct usbh_hubport *hport, struct usb_device_descriptor *desc, uint16_t length)
{
    if (desc->bLength != USB_SIZEOF_DEVICE_DESC) {
        //USB_LOG_ERR("invalid device bLength 0x%02x\r\n", desc->bLength);
        return -USB_ERR_INVAL;
    } else if (desc->bDescriptorType != USB_DESCRIPTOR_TYPE_DEVICE) {
        //USB_LOG_ERR("unexpected device descriptor 0x%02x\r\n", desc->bDescriptorType);
        return -USB_ERR_INVAL;
    } else {
        if (length <= 8) {
            return 0;
        }
        hport->device_desc.bLength = desc->bLength;
        hport->device_desc.bDescriptorType = desc->bDescriptorType;
        hport->device_desc.bcdUSB = desc->bcdUSB;
        hport->device_desc.bDeviceClass = desc->bDeviceClass;
        hport->device_desc.bDeviceSubClass = desc->bDeviceSubClass;
        hport->device_desc.bDeviceProtocol = desc->bDeviceProtocol;
        hport->device_desc.bMaxPacketSize0 = desc->bMaxPacketSize0;
        hport->device_desc.idVendor = desc->idVendor;
        hport->device_desc.idProduct = desc->idProduct;
        hport->device_desc.bcdDevice = desc->bcdDevice;
        hport->device_desc.iManufacturer = desc->iManufacturer;
        hport->device_desc.iProduct = desc->iProduct;
        hport->device_desc.iSerialNumber = desc->iSerialNumber;
        hport->device_desc.bNumConfigurations = desc->bNumConfigurations;
    }
    return 0;
}

static int parse_config_descriptor(struct usbh_hubport *hport, struct usb_configuration_descriptor *desc, uint16_t length)
{
    struct usb_interface_descriptor *intf_desc;
    struct usb_endpoint_descriptor *ep_desc;
    uint8_t cur_alt_setting = 0xff;
    uint8_t cur_iface = 0xff;
    uint8_t cur_ep = 0xff;
    uint8_t cur_ep_num = 0xff;
    uint32_t desc_len = 0;
    uint8_t *p;

    if (desc->bLength != USB_SIZEOF_CONFIG_DESC) 
    {
      return -USB_ERR_INVAL;
    } 
    else if (desc->bDescriptorType != USB_DESCRIPTOR_TYPE_CONFIGURATION) 
    {
      return -USB_ERR_INVAL;
    } 
    else 
    {
        if (length <= USB_SIZEOF_CONFIG_DESC) {
            return 0;
        }
        hport->config.config_desc.bLength = desc->bLength;
        hport->config.config_desc.bDescriptorType = desc->bDescriptorType;
        hport->config.config_desc.wTotalLength = desc->wTotalLength;
        hport->config.config_desc.bNumInterfaces = desc->bNumInterfaces;
        hport->config.config_desc.bConfigurationValue = desc->bConfigurationValue;
        hport->config.config_desc.iConfiguration = desc->iConfiguration;
        hport->config.config_desc.iConfiguration = desc->iConfiguration;
        hport->config.config_desc.bmAttributes = desc->bmAttributes;
        hport->config.config_desc.bMaxPower = desc->bMaxPower;

        p = (uint8_t *)desc;
        p += USB_SIZEOF_CONFIG_DESC;
        desc_len = USB_SIZEOF_CONFIG_DESC;

        memset(hport->config.intf, 0, sizeof(struct usbh_interface) * CONFIG_USBHOST_MAX_INTERFACES);

        while (p[DESC_bLength]) {
            switch (p[DESC_bDescriptorType]) {
                case USB_DESCRIPTOR_TYPE_INTERFACE:
                    if (p[DESC_bLength] < USB_SIZEOF_INTERFACE_DESC) {
                        return -USB_ERR_INVAL;
                    }
                    intf_desc = (struct usb_interface_descriptor *)p;
                    cur_iface = intf_desc->bInterfaceNumber;
                    cur_alt_setting = intf_desc->bAlternateSetting;
                    cur_ep_num = intf_desc->bNumEndpoints;
                    cur_ep = 0;

                    if (cur_iface >= CONFIG_USBHOST_MAX_INTERFACES) {
                        return -USB_ERR_NOMEM;
                    }

                    if (cur_ep_num > CONFIG_USBHOST_MAX_ENDPOINTS) {
                        return -USB_ERR_NOMEM;
                    }

                    if (cur_alt_setting >= CONFIG_USBHOST_MAX_INTF_ALTSETTINGS) {
                        return -USB_ERR_NOMEM;
                    }
                    memcpy(&hport->config.intf[cur_iface].altsetting[cur_alt_setting].intf_desc, intf_desc, 9);
                    hport->config.intf[cur_iface].altsetting_num = cur_alt_setting + 1;
                    break;
                case USB_DESCRIPTOR_TYPE_ENDPOINT:
                    if (p[DESC_bLength] < USB_SIZEOF_ENDPOINT_DESC) {
                        return -USB_ERR_INVAL;
                    }
                    ep_desc = (struct usb_endpoint_descriptor *)p;

                    if (cur_ep >= CONFIG_USBHOST_MAX_ENDPOINTS) 
                    {
                        return -USB_ERR_NOMEM;
                    }

                    memcpy(&hport->config.intf[cur_iface].altsetting[cur_alt_setting].ep[cur_ep].ep_desc, ep_desc, 7);
                    cur_ep++;
                    break;

                default:
                    break;
            }
            /* skip to next descriptor */
            desc_len += p[DESC_bLength];
            p += p[DESC_bLength];

            if (desc_len > length) {
                return -USB_ERR_INVAL;
            }
        }

        if (desc_len != length) {
            return -USB_ERR_INVAL;
        }
    }
    return 0;
}

static void usbh_print_setup(struct usb_setup_packet *setup)
{
    (void)setup;
}

static int usbh_get_default_mps(int speed)
{
    switch (speed) {
        case USB_SPEED_LOW: /* For low speed, we use 8 bytes */
            return 8;
        case USB_SPEED_FULL: /* For full or high speed, we use 64 bytes */
        case USB_SPEED_HIGH:
            return 64;
        case USB_SPEED_SUPER: /* For super speed , we must use 512 bytes */
        case USB_SPEED_SUPER_PLUS:
            return 512;
        default:
            return 64;
    }
}

int usbh_enumerate(struct usbh_hubport *hport)
{
    struct usb_interface_descriptor *intf_desc;
    struct usb_setup_packet *setup;
    struct usb_device_descriptor *dev_desc;
    struct usb_endpoint_descriptor *ep;
    int dev_addr;
    uint16_t ep_mps;
    uint8_t config_value;
    uint8_t config_index;
    int ret;

    hport->setup = (struct usb_setup_packet *)&g_setup_buffer[hport->bus->busid][hport->parent->index - 1][hport->port - 1];
    setup = hport->setup;
    ep = &hport->ep0;

    /* Config EP0 mps from speed */
    ep->bEndpointAddress = 0x00;
    ep->bDescriptorType = USB_DESCRIPTOR_TYPE_ENDPOINT;
    ep->bmAttributes = USB_ENDPOINT_TYPE_CONTROL;
    ep->wMaxPacketSize = usbh_get_default_mps(hport->speed);
    ep->bInterval = 0;
    ep->bLength = 7;

    /* Configure EP0 with zero address */
    hport->dev_addr = 0;

    /* Read the first 8 bytes of the device descriptor */
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_DEVICE << 8) | 0);
    setup->wIndex = 0;
    setup->wLength = 8;

    ret = usbh_control_transfer(hport, setup, ep0_request_buffer[hport->bus->busid]);
    if (ret < 8) 
    {
        goto errout;
    }

    ret = parse_device_descriptor(hport, (struct usb_device_descriptor *)ep0_request_buffer[hport->bus->busid], 8);
    if (ret < 0) 
    {
        goto errout;
    }

    /* Extract the correct max packetsize from the device descriptor */
    dev_desc = (struct usb_device_descriptor *)ep0_request_buffer[hport->bus->busid];
    if (dev_desc->bcdUSB >= USB_3_0) {
        ep_mps = 1 << dev_desc->bMaxPacketSize0;
    } else {
        ep_mps = dev_desc->bMaxPacketSize0;
    }

    /* Reconfigure EP0 with the correct maximum packet size */
    ep->wMaxPacketSize = ep_mps;

    /* Assign a function address to the device connected to this port */
    dev_addr = usbh_allocate_devaddr(&hport->bus->devgen);
    if (dev_addr < 0) {
        goto errout;
    }

    /* Set the USB device address */
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_SET_ADDRESS;
    setup->wValue = dev_addr;
    setup->wIndex = 0;
    setup->wLength = 0;

    ret = usbh_control_transfer(hport, setup, NULL);
    if (ret < 0) 
    {
        goto errout;
    }

    /* Wait device set address completely */
    usb_osal_msleep(10);

    /*Reconfigure EP0 with the correct address */
    hport->dev_addr = dev_addr;

    /* Read the full device descriptor */
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_DEVICE << 8) | 0);
    setup->wIndex = 0;
    setup->wLength = USB_SIZEOF_DEVICE_DESC;

    ret = usbh_control_transfer(hport, setup, ep0_request_buffer[hport->bus->busid]);
    if (ret < USB_SIZEOF_DEVICE_DESC) {
        goto errout;
    }

    parse_device_descriptor(hport, (struct usb_device_descriptor *)ep0_request_buffer[hport->bus->busid], USB_SIZEOF_DEVICE_DESC);    

    config_index = usbh_get_hport_active_config_index(hport);

    /* Read the first 9 bytes of the config descriptor */
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_CONFIGURATION << 8) | config_index);
    setup->wIndex = 0;
    setup->wLength = USB_SIZEOF_CONFIG_DESC;

    ret = usbh_control_transfer(hport, setup, ep0_request_buffer[hport->bus->busid]);
    if (ret < USB_SIZEOF_CONFIG_DESC) {
        goto errout;
    }

    ret = parse_config_descriptor(hport, (struct usb_configuration_descriptor *)ep0_request_buffer[hport->bus->busid], USB_SIZEOF_CONFIG_DESC);
    if (ret < 0) 
    {
        goto errout;
    }

    /* Read the full size of the configuration data */
    uint16_t wTotalLength = ((struct usb_configuration_descriptor *)ep0_request_buffer[hport->bus->busid])->wTotalLength;

    if (wTotalLength >= CONFIG_USBHOST_REQUEST_BUFFER_LEN) {
        ret = -USB_ERR_NOMEM;
        goto errout;
    }

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_CONFIGURATION << 8) | config_index);
    setup->wIndex = 0;
    setup->wLength = wTotalLength;

    ret = usbh_control_transfer(hport, setup, ep0_request_buffer[hport->bus->busid]);
    if (ret < wTotalLength) 
    {
        goto errout;
    }

    ep0_request_buffer[hport->bus->busid][wTotalLength] = '\0';
    ret = parse_config_descriptor(hport, (struct usb_configuration_descriptor *)ep0_request_buffer[hport->bus->busid], wTotalLength);
    if (ret < 0) {
        goto errout;
    }

    hport->raw_config_desc = usb_osal_malloc(wTotalLength + 1);
    if (hport->raw_config_desc == NULL) {
        ret = -USB_ERR_NOMEM;
        goto errout;
    }

    config_value = ((struct usb_configuration_descriptor *)ep0_request_buffer[hport->bus->busid])->bConfigurationValue;
    memcpy(hport->raw_config_desc, ep0_request_buffer[hport->bus->busid], wTotalLength);
    hport->raw_config_desc[wTotalLength] = '\0';

#ifdef CONFIG_USBHOST_GET_STRING_DESC
    uint8_t string_buffer[128];

    if (hport->device_desc.iManufacturer > 0) {
        /* Get Manufacturer string */
        memset(string_buffer, 0, 128);
        ret = usbh_get_string_desc(hport, USB_STRING_MFC_INDEX, string_buffer, 128);
        if (ret < 0) {
            goto errout;
        }
    } 
    else 
    {    
    }

    if (hport->device_desc.iProduct > 0) {
        /* Get Product string */
        memset(string_buffer, 0, 128);
        ret = usbh_get_string_desc(hport, USB_STRING_PRODUCT_INDEX, string_buffer, 128);
        if (ret < 0) 
        {
            goto errout;
        }

    } 
    else 
    {
    }

    if (hport->device_desc.iSerialNumber > 0) {
        /* Get SerialNumber string */
        memset(string_buffer, 0, 128);
        ret = usbh_get_string_desc(hport, USB_STRING_SERIAL_INDEX, string_buffer, 128);
        if (ret < 0) {
            USB_LOG_ERR("Failed to get SerialNumber string,errorcode:%d\r\n", ret);
            goto errout;
        }

        USB_LOG_INFO("SerialNumber: %s\r\n", string_buffer);
    } else {
        USB_LOG_WRN("Do not support SerialNumber string\r\n");
    }
#endif
    /* Select device configuration 1 */
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_SET_CONFIGURATION;
    setup->wValue = config_value;
    setup->wIndex = 0;
    setup->wLength = 0;

    ret = usbh_control_transfer(hport, setup, NULL);
    if (ret < 0) {
        goto errout;
    }

#ifdef CONFIG_USBHOST_MSOS_ENABLE
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = CONFIG_USBHOST_MSOS_VENDOR_CODE;
    setup->wValue = 0;
    setup->wIndex = 0x0004;
    setup->wLength = 16;

    ret = usbh_control_transfer(hport, setup, ep0_request_buffer[hport->bus->busid]);
    if (ret < 0 && (ret != -USB_ERR_STALL)) {
        goto errout;
    }
#endif
    hport->bus->event_handler(hport->bus->busid, hport->parent->index, hport->port, USB_INTERFACE_ANY, USBH_EVENT_DEVICE_CONFIGURED);
    /*search supported class driver*/
    for (uint8_t i = 0; i < hport->config.config_desc.bNumInterfaces; i++) {
        intf_desc = &hport->config.intf[i].altsetting[0].intf_desc;

        struct usbh_class_driver *class_driver = (struct usbh_class_driver *)usbh_find_class_driver(intf_desc->bInterfaceClass,
                                                                                                    intf_desc->bInterfaceSubClass,
                                                                                                    intf_desc->bInterfaceProtocol,
                                                                                                    intf_desc->bInterfaceNumber,
                                                                                                    hport->device_desc.idVendor,
                                                                                                    hport->device_desc.idProduct);

        if (class_driver == NULL) {
            hport->bus->event_handler(hport->bus->busid, hport->parent->index, hport->port, i, USBH_EVENT_INTERFACE_UNSUPPORTED);
            continue;
        }
        hport->config.intf[i].class_driver = class_driver;
        ret = CLASS_CONNECT(hport, i);
        if (ret >= 0) {
            hport->bus->event_handler(hport->bus->busid, hport->parent->index, hport->port, i, USBH_EVENT_INTERFACE_START);
        }
    }

errout:
    if (hport->raw_config_desc) 
    {
      usb_osal_free(hport->raw_config_desc);
      hport->raw_config_desc = NULL;
    }
    return ret;
}

void usbh_hubport_release(struct usbh_hubport *hport)
{
    if (hport->connected) {
        hport->connected = false;
        usbh_kill_urb(&hport->ep0_urb);
        usbh_free_devaddr(hport);
        for (uint8_t i = 0; i < hport->config.config_desc.bNumInterfaces; i++) {
            if (hport->config.intf[i].class_driver && hport->config.intf[i].class_driver->disconnect) {
                CLASS_DISCONNECT(hport, i);
            }
            hport->bus->event_handler(hport->bus->busid, hport->parent->index, hport->port, i, USBH_EVENT_INTERFACE_STOP);
        }
        hport->config.config_desc.bNumInterfaces = 0;
        usb_osal_mutex_take(hport->mutex);
        usb_osal_mutex_delete(hport->mutex);
        hport->bus->event_handler(hport->bus->busid, hport->parent->index, hport->port, USB_INTERFACE_ANY, USBH_EVENT_DEVICE_DISCONNECTED);
    }
}

static void usbh_bus_init(struct usbh_bus *bus, uint8_t busid, uintptr_t reg_base)
{
#if defined(CONFIG_USBHOST_MULT_HC)
  const struct usbh_hc_driver *hc_driver = bus->hc_driver;
#endif

  memset(bus, 0, sizeof(struct usbh_bus));
  bus->busid = busid;
  bus->hcd.hcd_id = busid;
  bus->hcd.reg_base = reg_base;
#if defined(CONFIG_USBHOST_MULT_HC)
  bus->hc_driver = hc_driver;
#endif
  /* devaddr 1 is for roothub */
  bus->devgen.last = 0x7f;

  usb_slist_add_tail(&g_bus_head, &bus->list);
}

int usbh_initialize(uint8_t busid, uintptr_t reg_base, usbh_event_handler_t event_handler)
{
  struct usbh_bus *bus;

  bus = &g_usbhost_bus[busid];      /* Get the structure address of the Bus data. Static allocation. */

  usbh_bus_init(bus, busid, reg_base);

  if (event_handler) 
  {
    bus->event_handler = event_handler;
  } 
  else 
  {
    bus->event_handler = dummy_event_handler;
  }

#ifdef __ARMCC_VERSION /* ARM C Compiler */
  extern const int usbh_class_info$$Base;
  extern const int usbh_class_info$$Limit;
  usbh_class_info_table_begin = (struct usbh_class_info *)&usbh_class_info$$Base;
  usbh_class_info_table_end = (struct usbh_class_info *)&usbh_class_info$$Limit;
#elif defined(__GNUC__)
  extern uint32_t __usbh_class_info_start__;
  extern uint32_t __usbh_class_info_end__;
  usbh_class_info_table_begin = (struct usbh_class_info *)&__usbh_class_info_start__;
  usbh_class_info_table_end = (struct usbh_class_info *)&__usbh_class_info_end__;
#elif defined(__ICCARM__) || defined(__ICCRX__) || defined(__ICCRISCV__)
  usbh_class_info_table_begin = (struct usbh_class_info *)__section_begin(".usbh_class_info");
  usbh_class_info_table_end = (struct usbh_class_info *)__section_end(".usbh_class_info");
#endif
  usbh_hub_initialize(bus);
  return 0;
}

int usbh_deinitialize(uint8_t busid)
{
    struct usbh_bus *bus;

    bus = &g_usbhost_bus[busid];

    usbh_hub_deinitialize(bus);

    usb_slist_remove(&g_bus_head, &bus->list);

    return 0;
}

int usbh_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup, uint8_t *buffer)
{
    struct usbh_urb *urb;
    volatile uint8_t retry = 3;
    int ret;

    if (!hport || !setup) {
        return -USB_ERR_INVAL;
    }

    if (!hport->connected) {
        return -USB_ERR_NODEV;
    }

    urb = &hport->ep0_urb;

    usb_osal_mutex_take(hport->mutex);

    usbh_print_setup(setup);

resubmit:
    usbh_control_urb_fill(urb, hport, setup, buffer, setup->wLength, CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT, NULL, NULL);
    ret = usbh_submit_urb(urb);
    if (ret == 0) {
        ret = urb->actual_length;
    }

    if (ret < (int)sizeof(struct usb_setup_packet) && (ret != -USB_ERR_TIMEOUT)) {
        retry--;
        if (retry > 0) {
            goto resubmit;
        }
    }

    usb_osal_mutex_give(hport->mutex);
    return ret < (int)sizeof(struct usb_setup_packet) ? ret : (ret - (int)sizeof(struct usb_setup_packet));
}

int usbh_get_string_desc(struct usbh_hubport *hport, uint8_t index, uint8_t *output, uint16_t output_len)
{
    struct usb_setup_packet *setup = hport->setup;
    int ret;
    uint8_t *src;
    uint8_t *dst;
    uint16_t len;
    uint16_t i = 2;
    uint16_t j = 0;

    /* Get Manufacturer string */
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_STRING << 8) | index);
    setup->wIndex = 0x0409;
    setup->wLength = 255;

    ret = usbh_control_transfer(hport, setup, ep0_request_buffer[hport->bus->busid]);
    if (ret < 0) {
        return ret;
    }

    src = ep0_request_buffer[hport->bus->busid];
    dst = output;
    len = src[0];

    if (ret != len) {
        return -USB_ERR_IO;
    }

    memset(output, 0, output_len);

    if (((len - 2) / 2) > output_len) {
        return -USB_ERR_NOMEM;
    }

    while (i < len) {
        dst[j] = src[i];
        i += 2;
        j++;
    }

    return 0;
}

int usbh_set_interface(struct usbh_hubport *hport, uint8_t intf, uint8_t altsetting)
{
    struct usb_setup_packet *setup = hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = USB_REQUEST_SET_INTERFACE;
    setup->wValue = altsetting;
    setup->wIndex = intf;
    setup->wLength = 0;

    return usbh_control_transfer(hport, setup, NULL);
}

static void *usbh_list_all_interface_name(struct usbh_hub *hub, const char *devname)
{
    struct usbh_hubport *hport;
    struct usbh_hub *hub_next;
    void *priv;

    for (uint8_t port = 0; port < hub->nports; port++) {
        hport = &hub->child[port];
        if (hport->connected) {
            for (uint8_t itf = 0; itf < hport->config.config_desc.bNumInterfaces; itf++) {
                if (hport->config.intf[itf].class_driver && hport->config.intf[itf].class_driver->driver_name) {
                    if ((strncmp(hport->config.intf[itf].devname, devname, CONFIG_USBHOST_DEV_NAMELEN) == 0) && hport->config.intf[itf].priv)
                        return hport->config.intf[itf].priv;

                    if (strcmp(hport->config.intf[itf].class_driver->driver_name, "hub") == 0) {
                        hub_next = hport->config.intf[itf].priv;

                        if (hub_next && hub_next->connected) {
                            priv = usbh_list_all_interface_name(hub_next, devname);
                            if (priv) {
                                return priv;
                            }
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

static struct usbh_hubport *usbh_list_all_hubport(struct usbh_hub *hub, uint8_t hub_index, uint8_t hub_port)
{
    struct usbh_hubport *hport;
    struct usbh_hub *hub_next;

    if (hub->index == hub_index) {
        hport = &hub->child[hub_port - 1];
        if (hport->connected) {
            return hport;
        } else {
            return NULL;
        }
    } else {
        for (uint8_t port = 0; port < hub->nports; port++) {
            hport = &hub->child[port];
            if (hport->connected) {
                for (uint8_t itf = 0; itf < hport->config.config_desc.bNumInterfaces; itf++) {
                    if (hport->config.intf[itf].class_driver && hport->config.intf[itf].class_driver->driver_name) {
                        if (strcmp(hport->config.intf[itf].class_driver->driver_name, "hub") == 0) {
                            hub_next = hport->config.intf[itf].priv;

                            if (hub_next && hub_next->connected) {
                                struct usbh_hubport *hport_next = usbh_list_all_hubport(hub_next, hub_index, hub_port);
                                if (hport_next) {
                                    return hport_next;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

void *usbh_find_class_instance(const char *devname)
{
    usb_slist_t *bus_list;
    struct usbh_hub *hub;
    struct usbh_bus *bus;
    void *priv;
    size_t flags;

    flags = usb_osal_enter_critical_section();
    usb_slist_for_each(bus_list, &g_bus_head)
    {
        bus = usb_slist_entry(bus_list, struct usbh_bus, list);
        hub = &bus->hcd.roothub;

        priv = usbh_list_all_interface_name(hub, devname);
        if (priv) {
            usb_osal_leave_critical_section(flags);
            return priv;
        }
    }
    usb_osal_leave_critical_section(flags);
    return NULL;
}

struct usbh_hubport *usbh_find_hubport(uint8_t busid, uint8_t hub_index, uint8_t hub_port)
{
    struct usbh_hub *hub;
    struct usbh_bus *bus;
    struct usbh_hubport *hport;
    size_t flags;

    flags = usb_osal_enter_critical_section();

    bus = &g_usbhost_bus[busid];
    hub = &bus->hcd.roothub;

    hport = usbh_list_all_hubport(hub, hub_index, hub_port);
    usb_osal_leave_critical_section(flags);
    return hport;
}

static void usbh_list_device(struct usbh_hub *hub, bool astree, bool verbose, int dev_addr, int vid, int pid)
{
    static const char *speed_table[] = {
        "UNKNOWN",
        "low-speed",
        "full-speed",
        "high-speed",
        "wireless",
        "super-speed",
        "super-speed-plus",
    };

    static const char *root_speed_table[] = {
        "UNKNOWN",
        "1.1",
        "1.1",
        "2.0",
        "2.5",
        "3.0",
        "3.0",
    };

    static const uint16_t speed_baud[] = {
        0,
        12,
        12,
        480,
        480,
        5000,
        10000,
    };

    struct usbh_bus *bus;
    struct usbh_hubport *hport;
    struct usbh_hub *hub_next;

    uint8_t imbuf[64];
    uint8_t ipbuf[64];

    const char *pimstr;
    const char *pipstr;

    bool imvalid = false;
    bool ipvalid = false;

    int ret;

    bus = hub->bus;

    (void)speed_table;

    if (hub->is_roothub) {
        if (astree) {


        } else {
            if ((dev_addr < 0) || (hub->hub_addr == dev_addr)) {
                if (((vid < 0) || (vid == 0xffff)) && ((pid < 0) || (pid == 0xffff))) {
                }
            }
        }
    }

    for (uint8_t port = 0; port < hub->nports; port++) {
        hport = &hub->child[port];
        if (hport->connected) {
            ret = 0;
            if (hport->device_desc.iManufacturer) {
                memset(imbuf, 0, sizeof(imbuf));
                ret = usbh_get_string_desc(hport, hport->device_desc.iManufacturer, imbuf, sizeof(imbuf));
                if (ret == 0) {
                    imvalid = true;
                }
            }

            if (hport->device_desc.iProduct) {
                memset(ipbuf, 0, sizeof(ipbuf));
                ret = usbh_get_string_desc(hport, hport->device_desc.iProduct, ipbuf, sizeof(ipbuf));
                if (ret == 0) {
                    ipvalid = true;
                }
            }

            if (imvalid) {
                pimstr = (const char *)imbuf;
            } else {
                pimstr = "Not specified Manufacturer";
            }

            if (ipvalid) {
                pipstr = (const char *)ipbuf;
            } else {
                pipstr = "Not specified Product";
            }

            if (!astree) {
                if ((dev_addr < 0) || (hport->dev_addr == dev_addr)) {
                    if (((vid < 0) || (vid == hport->device_desc.idVendor)) && ((pid < 0) || (pid == hport->device_desc.idProduct))) {

                    }
                }
            }

            for (uint8_t intf = 0; intf < hport->config.config_desc.bNumInterfaces; intf++) {
                if (hport->config.intf[intf].class_driver && hport->config.intf[intf].class_driver->driver_name) {
                    if (astree) {                        
                    }

                    if (!strcmp(hport->config.intf[intf].class_driver->driver_name, "hub")) {
                        hub_next = hport->config.intf[intf].priv;

                        if (hub_next && hub_next->connected) {
                            usbh_list_device(hub_next, astree, verbose, dev_addr, vid, pid);
                        }
                    }
                } else if (astree) {                    
                }
            }
        }
    }
}

__WEAK uint8_t usbh_get_hport_active_config_index(struct usbh_hubport *hport)
{
    ARG_UNUSED(hport);

    return 0; // Default to configuration index 0
}

#if defined(CONFIG_USBHOST_MULT_HC)
void usbh_register_hc_driver(uint8_t busid, const struct usbh_hc_driver *driver)
{
    USB_ASSERT_MSG(busid < CONFIG_USBHOST_MAX_BUS, "bus overflow\r\n");
    USB_ASSERT_MSG(driver != NULL, "hc driver driver is NULL\r\n");

    g_usbhost_bus[busid].hc_driver = driver;
}

int __WEAK usb_hc_init(struct usbh_bus *bus)
{
  if (bus && bus->hc_driver && bus->hc_driver->init) 
  {
    return bus->hc_driver->init(bus);
  } 
  else 
  {
    return -USB_ERR_INVAL;
  }
}

int usb_hc_deinit(struct usbh_bus *bus)
{
    if (bus && bus->hc_driver && bus->hc_driver->deinit) {
        return bus->hc_driver->deinit(bus);
    } else {
        return -USB_ERR_INVAL;
    }
}

uint16_t usbh_get_frame_number(struct usbh_bus *bus)
{
    if (bus && bus->hc_driver && bus->hc_driver->get_frame_number) {
        return bus->hc_driver->get_frame_number(bus);
    } else {
        return 0;
    }
}

int usbh_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf)
{
    if (bus && bus->hc_driver && bus->hc_driver->roothub_control) {
        return bus->hc_driver->roothub_control(bus, setup, buf);
    } else {
        return -USB_ERR_INVAL;
    }
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    if (urb && urb->hport && urb->hport->bus && urb->hport->bus->hc_driver && urb->hport->bus->hc_driver->submit_urb) {
        return urb->hport->bus->hc_driver->submit_urb(urb);
    } else {
        return -USB_ERR_INVAL;
    }
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    if (urb && urb->hport && urb->hport->bus && urb->hport->bus->hc_driver && urb->hport->bus->hc_driver->kill_urb) {
        return urb->hport->bus->hc_driver->kill_urb(urb);
    } else {
        return -USB_ERR_INVAL;
    }
}

void USBH_IRQHandler(uint8_t busid)
{
    USB_ASSERT_MSG(busid < CONFIG_USBHOST_MAX_BUS, "bus overflow\r\n");

    if (g_usbhost_bus[busid].hc_driver && g_usbhost_bus[busid].hc_driver->irq_handler) {
        g_usbhost_bus[busid].hc_driver->irq_handler(busid);
    }
}
#endif

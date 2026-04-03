#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"

#define _PID_MAP(itf, n) ((CFG_TUD_##itf) << (n))
#define USB_PID                                                                            \
    (0x4000 | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | _PID_MAP(HID, 2) | _PID_MAP(MIDI, 3) | \
     _PID_MAP(VENDOR, 4))

#define USB_VID 0xCafe
#define USB_BCD 0x0200

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+

tusb_desc_device_t const desc_device = { .bLength = sizeof(tusb_desc_device_t),
                                         .bDescriptorType = TUSB_DESC_DEVICE,
                                         .bcdUSB = USB_BCD,

                                         .bDeviceClass = 0x00,
                                         .bDeviceSubClass = 0x00,
                                         .bDeviceProtocol = 0x00,

                                         .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
                                         .idVendor = USB_VID,
                                         .idProduct = USB_PID,
                                         .bcdDevice = 0x0100,

                                         .iManufacturer = 0x01,
                                         .iProduct = 0x02,
                                         .iSerialNumber = 0x03,

                                         .bNumConfigurations = 0x01 };

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// HID Report Descriptor
//--------------------------------------------------------------------+

uint8_t const desc_hid_report[] = {
    // Standard keyboard
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),

    // Vendor-defined IN/OUT report for WebHID
    0x06, 0x00, 0xFF, // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01, // Usage (0x01)
    0xA1, 0x01, // Collection (Application)
    0x85, REPORT_ID_VENDOR, //   Report ID

    0x09, 0x02, //   Usage (0x02)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, VENDOR_REPORT_SIZE, //   Report Count (63)
    0x81, 0x02, //   Input (Data,Var,Abs)

    0x09, 0x03, //   Usage (0x03)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, VENDOR_REPORT_SIZE, //   Report Count (63)
    0x91, 0x02, //   Output (Data,Var,Abs)
    0xC0 // End Collection
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum { ITF_NUM_HID, ITF_NUM_TOTAL };

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define EPNUM_HID 0x81

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
                          100),

    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID,
                       CFG_TUD_HID_EP_BUFSIZE, 5)
};

#if TUD_OPT_HIGH_SPEED

uint8_t desc_other_speed_config[CONFIG_TOTAL_LEN];

tusb_desc_device_qualifier_t const desc_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = USB_BCD,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0x00
};

uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
    return (uint8_t const *)&desc_device_qualifier;
}

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;
    memcpy(desc_other_speed_config, desc_configuration, CONFIG_TOTAL_LEN);
    desc_other_speed_config[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return desc_other_speed_config;
}
#endif

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },
    "Karen",
    "RP2040 TOTP Key",
    NULL,
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    switch (index) {
    case STRID_LANGID:
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
        break;

    case STRID_SERIAL:
        chr_count = board_usb_get_serial(_desc_str + 1, 32);
        break;

    default:
        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
            return NULL;

        const char *str = string_desc_arr[index];
        chr_count = strlen(str);

        size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
        if (chr_count > max_count)
            chr_count = max_count;

        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
        break;
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

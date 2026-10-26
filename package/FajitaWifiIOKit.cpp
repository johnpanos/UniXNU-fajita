// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita Wi-Fi in the I/O Registry, so iOS's own network configuration takes en0 as a Wi-Fi interface (fajita-wifi-iokit=1).
//
// configd builds its interface list from IOKit, not from the ifnet list (open source configd-1241.140.6:
// SystemConfiguration.fproj/SCNetworkInterface.c and Plugins/InterfaceNamer/ifnamer.c): an object conforming to
// "IONetworkInterface" with IOInterfaceType, BSD Name, IOInterfaceNamePrefix/IOInterfaceUnit, IOBuiltin, IOPrimaryInterface and
// IOLocation, whose provider (the controller) carries IOMACAddress; a controller conforming to "IO80211Controller" makes it
// kSCNetworkInterfaceTypeIEEE80211 ("Wi-Fi"). That is why the bare BSD en0 (FajitaNet.cpp) was never configured.
//
// IONetworkingFamily, IO80211Family and IOSkywalkFamily are not in this kernel (ios16/board.toml prelink roots), so these are
// our own minimal classes with the names configd tests for (as FajitaPmic.cpp's AppleSmartBattery is for powerd): nothing links
// against Apple's classes and no Apple class layout is used. They describe the existing ifnet; the frames still go through
// FajitaNet (fajita-net=wifi) and the station (FajitaWifiDev.cpp).
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include <pexpert/pexpert.h>

extern "C" {
int fajita_wifi_iokit_publish(const char *bsd_name, unsigned unit, const uint8_t mac[6]);
volatile uint32_t fajita_wifi_iokit_state[4];   // [0] published, [1] controller registered, [2] interface registered, [3] error
}

class IO80211Controller : public IOService {
    OSDeclareDefaultStructors(IO80211Controller)
};
OSDefineMetaClassAndStructors(IO80211Controller, IOService)

class IONetworkInterface : public IOService {
    OSDeclareDefaultStructors(IONetworkInterface)
};
OSDefineMetaClassAndStructors(IONetworkInterface, IOService)

class IOEthernetInterface : public IONetworkInterface {
    OSDeclareDefaultStructors(IOEthernetInterface)
};
OSDefineMetaClassAndStructors(IOEthernetInterface, IONetworkInterface)

class IO80211Interface : public IOEthernetInterface {
    OSDeclareDefaultStructors(IO80211Interface)
};
OSDefineMetaClassAndStructors(IO80211Interface, IOEthernetInterface)

namespace {
IO80211Controller *gController;
IO80211Interface *gInterface;

void setNumber(IOService *s, const char *key, unsigned long long v, unsigned bits)
{
    OSNumber *n = OSNumber::withNumber(v, bits);
    if (n) {s->setProperty(key, n); n->release();}
}
void setString(IOService *s, const char *key, const char *v)
{
    OSString *str = OSString::withCString(v);
    if (str) {s->setProperty(key, str); str->release();}
}
}  // namespace

// Called once en0 is attached (FajitaNet.cpp, fajita-net=wifi) with its BSD name, unit and MAC. 0 when both objects are registered.
extern "C" int fajita_wifi_iokit_publish(const char *bsd_name, unsigned unit, const uint8_t mac[6])
{
    unsigned on = 0;
    if (!PE_parse_boot_argn("fajita-wifi-iokit", &on, sizeof on) || !on) return -1;
    if (gInterface) return 0;
    IOService *platform = IOService::getPlatform();
    if (!platform) {fajita_wifi_iokit_state[3] = 1; return -2;}
    gController = OSTypeAlloc(IO80211Controller);
    if (!gController || !gController->init()) {fajita_wifi_iokit_state[3] = 2; return -3;}
    OSData *addr = OSData::withBytes(mac, 6);
    if (addr) {gController->setProperty("IOMACAddress", addr); addr->release();}
    setString(gController, "IOVendor", "Qualcomm");
    setString(gController, "IOModel", "WCN3990");
    setNumber(gController, "IOMaxPacketSize", 1514, 32);
    setNumber(gController, "IOLinkStatus", 3, 32);   // kIONetworkLinkValid | kIONetworkLinkActive
    if (!gController->attach(platform)) {fajita_wifi_iokit_state[3] = 3; return -4;}
    gController->registerService();
    fajita_wifi_iokit_state[1] = 1;

    gInterface = OSTypeAlloc(IO80211Interface);
    if (!gInterface || !gInterface->init()) {fajita_wifi_iokit_state[3] = 4; return -5;}
    setString(gInterface, "BSD Name", bsd_name);
    setString(gInterface, "IOInterfaceName", bsd_name);
    setString(gInterface, "IOInterfaceNamePrefix", "en");
    setNumber(gInterface, "IOInterfaceUnit", unit, 32);
    setNumber(gInterface, "IOInterfaceType", 6, 32);   // IFT_ETHER: SCNetworkInterface's IEEE 802.11 case
    setNumber(gInterface, "IOMaxTransferUnit", 1500, 32);
    setNumber(gInterface, "IOMediaAddressLength", 6, 32);
    setNumber(gInterface, "IOMediaHeaderLength", 14, 32);
    gInterface->setProperty("IOBuiltin", kOSBooleanTrue);
    gInterface->setProperty("IOPrimaryInterface", kOSBooleanTrue);
    setString(gInterface, "IOLocation", "");
    if (!gInterface->attach(gController)) {fajita_wifi_iokit_state[3] = 5; return -6;}
    gInterface->registerService();
    fajita_wifi_iokit_state[2] = 1;
    fajita_wifi_iokit_state[0] = 1;
    IOLog("fajita-wifi: FW8 iokit: IO80211Controller + IO80211Interface (%s, unit %u) registered\n", bsd_name, unit);
    return 0;
}

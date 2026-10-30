// Bluetooth LE (nn::bluetooth over the bt / btm services): Pokémon Violet initializes BLE in nnMain for its Pokémon GO link (main+0xa0b02c), then takes the five BLE events, without checking
// a result; the SDK's InitializeBluetoothUserInterface aborts when it cannot connect to bt (an nn::diag abort from sdk+0xaec1c), and each Acquire…Event aborts in btm::user without a session.
// The host has no Bluetooth: the initialization is a no-op and the events are bound to kernel events that never fire (no device is ever found); the scan, connection and GATT calls are taken over
// as games come to use them.
#include "hle.h"

HLE_FORCE("_ZN2nn9bluetooth13InitializeBleEv", "connect to the bt (Bluetooth LE) service; the host has no Bluetooth, so there is nothing to connect to", Result, h_bt_InitializeBle, void) {
    LOG("init takeover: bluetooth::InitializeBle (no-op)"); return RES_OK;
}
HLE_FORCE("_ZN2nn9bluetooth19AcquireBleScanEventEPNS_2os15SystemEventTypeE", "BLE scan event: bound, never fires (no Bluetooth on the host)", Result, h_bt_ScanEv, void *ev) {
    hle_bind_system_event(ev, 0); return RES_OK;
}
HLE_FORCE("_ZN2nn9bluetooth37AcquireBleConnectionStateChangedEventEPNS_2os15SystemEventTypeE", "BLE connection state event: bound, never fires", Result, h_bt_ConnEv, void *ev) {
    hle_bind_system_event(ev, 0); return RES_OK;
}
HLE_FORCE("_ZN2nn9bluetooth31AcquireBleServiceDiscoveryEventEPNS_2os15SystemEventTypeE", "BLE service discovery event: bound, never fires", Result, h_bt_DiscEv, void *ev) {
    hle_bind_system_event(ev, 0); return RES_OK;
}
HLE_FORCE("_ZN2nn9bluetooth24AcquireBleMtuConfigEventEPNS_2os15SystemEventTypeE", "BLE MTU configuration event: bound, never fires", Result, h_bt_MtuEv, void *ev) {
    hle_bind_system_event(ev, 0); return RES_OK;
}
HLE_FORCE("_ZN2nn9bluetooth28AcquireBleGattOperationEventEPNS_2os15SystemEventTypeE", "BLE GATT operation event: bound, never fires", Result, h_bt_GattEv, void *ev) {
    hle_bind_system_event(ev, 0); return RES_OK;
}

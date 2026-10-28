// Bluetooth LE for game accessories (nn::bluetooth, through the btm / bt system services): no accessory is ever present. Pokémon Scarlet 4.0.0 calls InitializeBle and then acquires the LE events
// while it boots (main+0xa0b02c, under nnMain), for the Pokémon GO Plus +; the original InitializeBle connects to the bt service and the host has none ("[RED] sf: the game connects to service bt
// itself", then an abort in nn::bluetooth::hal::user::InitializeBluetoothUserInterface). Only entry points that reach btm / bt are hooked; GattService / GattCharacteristic / GattDescriptor objects
// and their getters only touch the game's own memory and keep running in the SDK.
// Return convention as in hle_offline.c: events get a bound SystemEvent that never fires, scans start and find nothing, connections and GATT operations fail.
#include "hle.h"
#include <stdbool.h>

HLE_FORCE("_ZN2nn9bluetooth13InitializeBleEv", "Bluetooth LE init (bt service); no accessories, no-op", void, h_bt_InitBle, void) {}

// The SDK's Acquire*Event return a Result on newer SDKs and nothing on older ones; 0 suits both
HLE_FORCE("_ZN2nn9bluetooth19AcquireBleScanEventEPNS_2os15SystemEventTypeE", "BLE scan event: bound, never fires", Result, h_bt_EvScan, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth37AcquireBleConnectionStateChangedEventEPNS_2os15SystemEventTypeE", "BLE connection event: bound, never fires", Result, h_bt_EvConn, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth31AcquireBleServiceDiscoveryEventEPNS_2os15SystemEventTypeE", "BLE service discovery event: bound, never fires", Result, h_bt_EvDisc, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth24AcquireBleMtuConfigEventEPNS_2os15SystemEventTypeE", "BLE MTU event: bound, never fires", Result, h_bt_EvMtu, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth28AcquireBleGattOperationEventEPNS_2os15SystemEventTypeE", "BLE GATT operation event: bound, never fires", Result, h_bt_EvGatt, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }

// Scans: the SDK aborts when the scan parameter lookup fails (GetBleScanParameter → btm GetBleScanFilterParameter), so it succeeds with an empty UUID (GattAttributeUuid: length word + 16 bytes)
HLE_FORCE("_ZN2nn9bluetooth19GetBleScanParameterEPNS0_17GattAttributeUuidEi", "BLE scan filter parameter (btm): an empty UUID", Result, h_bt_ScanParam, void *uuid, int id) { (void)id; if (uuid) memset(uuid, 0, 0x14); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth23StartBleScanSmartDeviceERKNS0_17GattAttributeUuidE", "BLE scan start (btm): started, finds nothing", Result, h_bt_ScanStart, const void *uuid) { (void)uuid; return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth22StopBleScanSmartDeviceEv", "BLE scan stop (btm)", Result, h_bt_ScanStop, void) { return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth16GetBleScanResultEPNS0_13BleScanResultEi", "BLE scan results (btm): 0 found", int, h_bt_ScanRes, void *out, int max) { (void)out; (void)max; return 0; }
HLE_FORCE("_ZN2nn9bluetooth24GetBleConnectionInfoListEPNS0_17BleConnectionInfoEi", "BLE connections (btm): none", int, h_bt_ConnList, void *out, int max) { (void)out; (void)max; return 0; }

// Connections and GATT: nothing to connect to
HLE_FORCE("_ZN2nn9bluetooth19ConnectToGattServerERKNS0_7AddressE", "BLE connect (btm): no accessory, fails", Result, h_bt_Connect, const void *addr) { (void)addr; return RES_FAIL; }
HLE_FORCE("_ZN2nn9bluetooth24DisconnectFromGattServerEj", "BLE disconnect (btm)", Result, h_bt_Disconnect, uint32_t h) { (void)h; return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth15ConfigureBleMtuEjt", "BLE MTU (btm): no connection, fails", Result, h_bt_Mtu, uint32_t h, uint16_t mtu) { (void)h; (void)mtu; return RES_FAIL; }
HLE_FORCE("_ZN2nn9bluetooth14GetGattServiceEPNS0_11GattServiceERKNS0_17GattAttributeUuidEj", "GATT service lookup (btm): none", bool, h_bt_GetSvc, void *out, const void *uuid, uint32_t h) { (void)out; (void)uuid; (void)h; return false; }
HLE_FORCE("_ZN2nn9bluetooth22ReadGattCharacteristicERKNS0_18GattCharacteristicE", "GATT read (bt): no connection, fails", Result, h_bt_ReadChr, const void *c) { (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn9bluetooth23WriteGattCharacteristicERKNS0_18GattCharacteristicE", "GATT write (bt): no connection, fails", Result, h_bt_WriteChr, const void *c) { (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn9bluetooth19WriteGattDescriptorERKNS0_14GattDescriptorE", "GATT descriptor write (bt): no connection, fails", Result, h_bt_WriteDsc, const void *d) { (void)d; return RES_FAIL; }
HLE_FORCE("_ZN2nn9bluetooth36EnableGattCharacteristicNotificationERKNS0_18GattCharacteristicEb", "GATT notification enable (bt): no connection, fails", Result, h_bt_Notify, const void *c, bool on) { (void)c; (void)on; return RES_FAIL; }
HLE_FORCE("_ZN2nn9bluetooth33RegisterGattOperationNotificationERKNS0_17GattAttributeUuidE", "GATT operation notification filter (btm)", Result, h_bt_RegNotify, const void *uuid) { (void)uuid; return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth35UnregisterGattOperationNotificationERKNS0_17GattAttributeUuidE", "GATT operation notification filter (btm)", Result, h_bt_UnregNotify, const void *uuid) { (void)uuid; return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth22GetGattOperationResultEPNS0_26BleClientGattOperationInfoE", "GATT operation result (bt): none pending, fails", Result, h_bt_GattRes, void *out) { (void)out; return RES_FAIL; }

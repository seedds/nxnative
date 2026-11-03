// SDK calls only Switch 2 (Ounce) programs make. The SDK's own versions reach services the host does not serve (sf aborts with 0xe15), so they are taken over at the public entry.
// No hardware behind these: no Joy-Con mouse mode, no Joy-Con camera (Jcms), no GameChat. Each answers "nothing there"; a game that needs more stops at its own check. Open: return types come from the
// mangled names, which do not carry them, so a Result and a void both answer 0 here.
#include "hle.h"
#include <string.h>

// nn::hid: Joy-Con camera / motion sensors (Jcms) and the Joy-Con's mouse mode (Nmouse)
HLE_FORCE("_ZN2nn3hid14InitializeJcmsEPvm", "Switch 2 hid: no Jcms device", void, h_nx2_JcmsInit, void *p, uint64_t n) { (void)p; (void)n; }
HLE_FORCE("_ZN2nn3hid12FinalizeJcmsEv", "Switch 2 hid: no Jcms device", void, h_nx2_JcmsFini, void) {}
HLE_FORCE("_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", "Switch 2 hid: no Jcms device", uint64_t, h_nx2_JcmsHandle, const void *id, const void *type) { (void)id; (void)type; return 0; }
HLE_FORCE("_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE", "Switch 2 hid: no Jcms device", int, h_nx2_JcmsStates, void *out, int n, const void *h) { (void)out; (void)n; (void)h; return 0; }
HLE_FORCE("_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", "Switch 2 hid: no Jcms device", void, h_nx2_JcmsStop, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid16InitializeNmouseEPvmii", "Switch 2 hid: no mouse mode", void, h_nx2_MouseInit, void *p, uint64_t n, int a, int b) { (void)p; (void)n; (void)a; (void)b; }
HLE_FORCE("_ZN2nn3hid14FinalizeNmouseEv", "Switch 2 hid: no mouse mode", void, h_nx2_MouseFini, void) {}
HLE_FORCE("_ZN2nn3hid16GetNmouseHandlesEPNS0_12NmouseHandleEij", "Switch 2 hid: no mouse mode", int, h_nx2_MouseHandles, void *out, int n, uint32_t style) { (void)out; (void)n; (void)style; return 0; }
HLE_FORCE("_ZN2nn3hid15GetNmouseStatesEPiPNS0_11NmouseStateEiNS0_12NmouseHandleE", "Switch 2 hid: no mouse mode", void, h_nx2_MouseStates, int *count, void *out, int n, uint32_t h) { (void)out; (void)n; (void)h; if (count) *count = 0; }
HLE_FORCE("_ZN2nn3hid16GetNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEENS0_12NmouseHandleE", "Switch 2 hid: no mouse mode", void, h_nx2_MouseTypes, uint32_t *out, uint32_t h) { (void)h; if (out) *out = 0; }
HLE_FORCE("_ZN2nn3hid19EnableNmousePointerENS0_12NmouseHandleEb", "Switch 2 hid: no mouse mode", void, h_nx2_MouseEnable, uint32_t h, uint32_t on) { (void)h; (void)on; }
HLE_FORCE("_ZN2nn3hid22SetNmousePointerRangesEii", "Switch 2 hid: no mouse mode", void, h_nx2_MouseRanges, int w, int h) { (void)w; (void)h; }
HLE_FORCE("_ZN2nn3hid24SetNmousePointerPositionENS0_12NmouseHandleEii", "Switch 2 hid: no mouse mode", void, h_nx2_MousePos, uint32_t h, int x, int y) { (void)h; (void)x; (void)y; }

// nn::oe: GetCurrentGraphicsScale answers 10, 20 or 30 and SDK code stops on anything else; 10 is the one seen working
HLE_FORCE("_ZN2nn2oe23GetCurrentGraphicsScaleEv", "Switch 2 oe: graphics scale 10", int, h_nx2_GfxScale, void) { return 10; }
HLE_FORCE("_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", "Switch 2 oe: the scale never changes", void, h_nx2_GfxScaleNotify, uint32_t on) { (void)on; }
HLE_FORCE("_ZN2nn2oe32BeginPresentPhotographedContentsEv", "Switch 2 oe: no capture restrictions here", void, h_nx2_PhotoBegin, void) {}
HLE_FORCE("_ZN2nn2oe30EndPresentPhotographedContentsEv", "Switch 2 oe: no capture restrictions here", void, h_nx2_PhotoEnd, void) {}

// nn::chat: GameChat is off, nobody is chatting
HLE_FORCE("_ZN2nn4chat10InitializeEv", "GameChat: off", uint32_t, h_nx2_ChatInit, void) { return 0; }
HLE_FORCE("_ZN2nn4chat10IsChattingEv", "GameChat: nobody chatting", uint32_t, h_nx2_ChatIs, void) { return 0; }
HLE_FORCE("_ZN2nn4chat11GetRoomInfoEPNS0_8RoomInfoE", "GameChat: no room", uint32_t, h_nx2_ChatRoom, void *out) { (void)out; return 0; }
HLE_FORCE("_ZN2nn4chat13SetSharingKeyERKNS0_10SharingKeyE", "GameChat: off", void, h_nx2_ChatKeySet, const void *k) { (void)k; }
HLE_FORCE("_ZN2nn4chat15ClearSharingKeyEv", "GameChat: off", void, h_nx2_ChatKeyClr, void) {}
HLE_FORCE("_ZN2nn4chat13ShowChatStartERKNS_7account3UidE", "GameChat: off", void, h_nx2_ChatShow, const void *u) { (void)u; }
HLE_FORCE("_ZN2nn4chat20SetScreenSharingModeENS0_17ScreenSharingModeE", "GameChat: off", void, h_nx2_ChatShare, int m) { (void)m; }
HLE_FORCE("_ZN2nn4chat22ShowCameraSharingGuideEv", "GameChat: off", void, h_nx2_ChatGuide, void) {}
HLE_FORCE("_ZN2nn4chat28AttachToRoomInfoChangedEventEPNS_2os11SystemEventE", "GameChat: bound, never signaled", uint32_t, h_nx2_ChatRoomEv, void *ev) { hle_bind_system_event(ev, 0); return 0; }
HLE_FORCE("_ZN2nn4chat36ShowLocalCameraConfigurationVersion0ENS0_25CameraImageProcessingModeE", "GameChat: no camera", void, h_nx2_ChatCfg, int m) { (void)m; }
HLE_FORCE("_ZN2nn4chat46BeginFullScreenWithLocalCameraAvailableSectionEb", "GameChat: no camera", void, h_nx2_ChatFsBegin, uint32_t b) { (void)b; }
HLE_FORCE("_ZN2nn4chat44EndFullScreenWithLocalCameraAvailableSectionEv", "GameChat: no camera", void, h_nx2_ChatFsEnd, void) {}
HLE_FORCE("_ZN2nn4chat25StartLocalCameraStreamingEv", "GameChat: no camera", void, h_nx2_ChatCamOn, void) {}
HLE_FORCE("_ZN2nn4chat24StopLocalCameraStreamingEv", "GameChat: no camera", void, h_nx2_ChatCamOff, void) {}
HLE_FORCE("_ZN2nn4chat26StartRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", "GameChat: no camera", void, h_nx2_ChatRemOn, const void *i) { (void)i; }
HLE_FORCE("_ZN2nn4chat25StopRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", "GameChat: no camera", void, h_nx2_ChatRemOff, const void *i) { (void)i; }
// The game asks for camera video storage at start and builds native images over it (below); a page keeps its size checks happy, and no frame ever arrives
HLE_FORCE("_ZN2nn4chat35CalculateLocalCameraVideoBufferSizeEiii", "GameChat: no camera, one page", uint64_t, h_nx2_ChatLSize, int w, int h, int n) { (void)w; (void)h; (void)n; return 0x1000; }
HLE_FORCE("_ZN2nn4chat36CalculateRemoteCameraVideoBufferSizeEiii", "GameChat: no camera, one page", uint64_t, h_nx2_ChatRSize, int w, int h, int n) { (void)w; (void)h; (void)n; return 0x1000; }
HLE_FORCE("_ZN2nn4chat33InitializeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeEiiiPvm", "GameChat: no camera", void, h_nx2_ChatLInit, void *t, int w, int h, int n, void *p, uint64_t sz) { (void)t; (void)w; (void)h; (void)n; (void)p; (void)sz; }
HLE_FORCE("_ZN2nn4chat34InitializeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeEiiiPvm", "GameChat: no camera", void, h_nx2_ChatRInit, void *t, int w, int h, int n, void *p, uint64_t sz) { (void)t; (void)w; (void)h; (void)n; (void)p; (void)sz; }
HLE_FORCE("_ZN2nn4chat31FinalizeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeE", "GameChat: no camera", void, h_nx2_ChatLFini, void *t) { (void)t; }
HLE_FORCE("_ZN2nn4chat32FinalizeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeE", "GameChat: no camera", void, h_nx2_ChatRFini, void *t) { (void)t; }
HLE_FORCE("_ZN2nn4chat25GetLocalCameraImageHandleEPKNS0_20LocalCameraVideoTypeEi", "GameChat: no camera", uint64_t, h_nx2_ChatLHandle, const void *t, int i) { (void)t; (void)i; return 0; }
HLE_FORCE("_ZN2nn4chat26GetRemoteCameraImageHandleEPKNS0_21RemoteCameraVideoTypeEi", "GameChat: no camera", uint64_t, h_nx2_ChatRHandle, const void *t, int i) { (void)t; (void)i; return 0; }
HLE_FORCE("_ZN2nn4chat27RequestNextLocalCameraImageEPNS_2gs16NativeSyncHandleEPNS0_20LocalCameraVideoTypeEiS2_", "GameChat: no camera", uint32_t, h_nx2_ChatLNext, void *a, void *t, int i, void *b) { (void)a; (void)t; (void)i; (void)b; return 0; }
HLE_FORCE("_ZN2nn4chat28RequestNextRemoteCameraImageEPNS_2gs16NativeSyncHandleEPNS0_21RemoteCameraVideoTypeEiS2_RKNS_7account23NetworkServiceAccountIdE", "GameChat: no camera", uint32_t, h_nx2_ChatRNext, void *a, void *t, int i, void *b, const void *c) { (void)a; (void)t; (void)i; (void)b; (void)c; return 0; }

// Native images and memory come from the graphics servers, which do not run here. Only the camera storage above builds them so far. Open: nvnTextureInitializeFromNativeImage leaves the texture
// uninitialized, so a game that samples one reaches a texture the host never made
HLE_FORCE("_Z32nvnNativeImageBuilderSetDefaultsP21NVNnativeImageBuilder", "NVN2 native image: no server", void, h_nx2_NiDef, void *b) { (void)b; }
HLE_FORCE("_Z30nvnNativeImageBuilderSetDeviceP21NVNnativeImageBuilderP9NVNdevice", "NVN2 native image: no server", void, h_nx2_NiDev, void *b, void *d) { (void)b; (void)d; }
HLE_FORCE("_Z30nvnNativeImageBuilderSetFormatP21NVNnativeImageBuilder9NVNformat", "NVN2 native image: no server", void, h_nx2_NiFmt, void *b, int f) { (void)b; (void)f; }
HLE_FORCE("_Z40nvnNativeImageBuilderSetMemoryPermissionP21NVNnativeImageBuilder30NVNnativeImageMemoryPermission", "NVN2 native image: no server", void, h_nx2_NiPerm, void *b, int p) { (void)b; (void)p; }
HLE_FORCE("_Z41nvnNativeImageBuilderSetNativeImageHandleP21NVNnativeImageBuilderN2nn2gs17NativeImageHandleE", "NVN2 native image: no server", void, h_nx2_NiHandle, void *b, uint64_t h) { (void)b; (void)h; }
HLE_FORCE("_Z24nvnNativeImageInitializeP14NVNnativeImagePK21NVNnativeImageBuilder", "NVN2 native image: no server", uint32_t, h_nx2_NiInit, void *i, const void *b) { (void)i; (void)b; return 1; }
HLE_FORCE("_Z22nvnNativeImageFinalizeP14NVNnativeImage", "NVN2 native image: no server", void, h_nx2_NiFini, void *i) { (void)i; }
HLE_FORCE("_Z35nvnTextureInitializeFromNativeImageP10NVNtextureP14NVNnativeImage", "NVN2 native image: no server", uint32_t, h_nx2_NiTex, void *t, void *i) { (void)t; (void)i; return 1; }
HLE_FORCE("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", "NVN2 native sync: already done", uint32_t, h_nx2_SyncTry, const void *h) { (void)h; return 1; }
HLE_FORCE("_ZN2nn2gs3sdk30CheckGraphicsSystemInitializedEPKcm", "the graphics servers do not run here", void, h_nx2_GsCheck, const char *s, uint64_t n) { (void)s; (void)n; }

// nn::camera: no camera. The game creates a context at start, and a non-zero Result stops it (main+0xcdec08)
HLE_FORCE("_ZN2nn6camera3sdk22CreateOutputBufferPoolEPNS_3gsv15UserImageHandleERKNS2_7imgdesc4impl4user5ounce25CameraUserImagePreferenceEjNS2_16UserMemoryHandleE", "Switch 2 camera: no server", uint32_t, h_nx2_CamPool, void *h, const void *p, uint32_t n, uint64_t m) { (void)h; (void)p; (void)n; (void)m; return 0; }
HLE_FORCE("_ZN2nn6camera3sdk23CalculateImageArraySizeEPNS_3gsv10MemorySizeES4_RKNS2_7imgdesc4impl4user5ounce25CameraUserImagePreferenceEj", "Switch 2 camera: no server, one page", uint32_t, h_nx2_CamSize, uint64_t *a, uint64_t *b, const void *p, uint32_t n) { (void)p; (void)n; if (a) *a = 0x1000; if (b) *b = 0x1000; return 0; }
HLE_FORCE("_ZN2nn6camera3sdk23DestroyOutputBufferPoolENS_3gsv15UserImageHandleE", "Switch 2 camera: no server", void, h_nx2_CamPoolFree, uint64_t h) { (void)h; }
HLE_FORCE("_ZN2nn6camera3sdk28DestroyAliasOutputBufferPoolENS_3gsv15UserImageHandleE", "Switch 2 camera: no server", void, h_nx2_CamAliasFree, uint64_t h) { (void)h; }
HLE_FORCE("_ZN2nn2gs3sdk18CreateNativeMemoryEPNS0_18NativeMemoryHandleEPvmNS_2os16MemoryPermissionENS0_3cpu18MemoryCacheabilityE", "NVN2 native memory: no server", uint32_t, h_nx2_NmCreate, uint64_t *h, void *p, uint64_t n, int perm, int cache) { (void)p; (void)n; (void)perm; (void)cache; if (h) *h = 1; return 0; }
HLE_FORCE("_ZN2nn2gs3sdk19DestroyNativeMemoryENS0_18NativeMemoryHandleE", "NVN2 native memory: no server", void, h_nx2_NmDestroy, uint64_t h) { (void)h; }

// nn::audio: the device service sits behind aud:u, which hands out a session the host does not make. The renderer itself runs locally
HLE_FORCE("_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv", "Switch 2 audio device: auto-tune off", uint32_t, h_nx2_AudTune, void) { return 0; }
HLE_FORCE("_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", "Switch 2 audio device: auto-tune off", void, h_nx2_AudTuneSet, uint32_t on) { (void)on; }
HLE_FORCE("_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", "Switch 2 audio device: one output", void, h_nx2_AudDev, char *name) { strlcpy(name, "MainAudioOut", 0x100); }
HLE_FORCE("_ZN2nn5audio32GetVirtualOutputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", "Switch 2 audio device: no properties", void, h_nx2_AudProps, void *out, const void *name) { (void)out; (void)name; }

// nn::vi: an SDR display with no variable refresh, so HDR metadata and VRR requests have nothing to act on
HLE_FORCE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", "Switch 2 vi: no HDR", uint32_t, h_nx2_HdrStatus, uint8_t *on, int *fmt, uint16_t *lum, const void *d) { (void)d; if (on) *on = 0; if (fmt) *fmt = 0; if (lum) *lum = 0; return 0; }
HLE_FORCE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", "Switch 2 vi: no HDR", uint32_t, h_nx2_HdrWhite, uint16_t *w, const void *d) { (void)d; if (w) *w = 0; return 0; }
HLE_FORCE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", "Switch 2 vi: no VRR", uint32_t, h_nx2_VrrOk, uint8_t *on, const void *d) { (void)d; if (on) *on = 0; return 0; }
HLE_FORCE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", "Switch 2 vi: no VRR", uint32_t, h_nx2_VrrSet, void *d, uint32_t on) { (void)d; (void)on; return 0; }
HLE_FORCE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", "Switch 2 vi: no VRR", uint32_t, h_nx2_SwapSet, void *l, int a, int b) { (void)l; (void)a; (void)b; return 0; }
HLE_FORCE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", "Switch 2 vi: no VRR", uint32_t, h_nx2_SwapClr, void *l) { (void)l; return 0; }
HLE_FORCE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", "Switch 2 vi: no HDR", uint32_t, h_nx2_HdrGamut, void *l, int g) { (void)l; (void)g; return 0; }
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", "Switch 2 vi: no HDR", uint32_t, h_nx2_HdrTone, void *l, int t) { (void)l; (void)t; return 0; }
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", "Switch 2 vi: no HDR", uint32_t, h_nx2_HdrBez, void *l, float a, float b, float c) { (void)l; (void)a; (void)b; (void)c; return 0; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", "Switch 2 vi: no HDR", uint32_t, h_nx2_HdrExt, void *l, uint32_t a, uint32_t b) { (void)l; (void)a; (void)b; return 0; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", "Switch 2 vi: no HDR", uint32_t, h_nx2_HdrStd, void *l, uint32_t a, uint32_t b) { (void)l; (void)a; (void)b; return 0; }

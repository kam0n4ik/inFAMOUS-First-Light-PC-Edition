# CALLS — реальные вызовы импортов (Phase 0)

Источник: `E:\SecondSonPC\logs\runs\owncs_full2\calls.json` (снимок 2026-10-18 23:27:58). Счётчиком обёрнуто 413 функций: native 336, observed 0, bridge 59 (весь HLE-реестр shadPS4).

Прогон: tag=owncs_full2 lang='' seconds=120

## Владельцы 417 функций, которые должен предоставить рантайм

| Владелец | Всего | Вызваны в прогоне |
|---|---:|---:|
| NATIVE | 336 | 163 |
| OBSERVED | 0 | 0 |
| BRIDGE | 61 | 8 |
| COMMON_STUB | 20 | 1 |
| **Итого** | **417** | **172** |
| LLE (модули игры, не входят в 417) | 147 | не считаются |

OBSERVED — наблюдатель SecondSonPC перед реализацией shadPS4; для миграции это всё ещё BRIDGE.
COMMON_STUB-вызовы счётчик не видит (shadPS4 пишет их в `logs\bridge_shadps4.log`: `CommonStub`).

## Вызванные функции (по убыванию)

| # | Функция | Библиотека | Владелец | Вызовов |
|---:|---|---|---|---:|
| 1 | `sceGnmDrawIndex` | libSceGnmDriver | NATIVE | 27570582 |
| 2 | `scePthreadMutexLock` | libkernel | NATIVE | 10974485 |
| 3 | `scePthreadMutexUnlock` | libkernel | NATIVE | 10967931 |
| 4 | `sceGnmUpdateVsShader` | libSceGnmDriver | NATIVE | 3540338 |
| 5 | `sceGnmUpdatePsShader350` | libSceGnmDriver | NATIVE | 3223028 |
| 6 | `sceGnmSetPsShader350` | libSceGnmDriver | NATIVE | 1559491 |
| 7 | `sceGnmSetVsShader` | libSceGnmDriver | NATIVE | 1242181 |
| 8 | `sceGnmDispatchDirect` | libSceGnmDriver | NATIVE | 1214051 |
| 9 | `sceGnmDrawIndexIndirect` | libSceGnmDriver | NATIVE | 1155030 |
| 10 | `sceGnmSetCsShaderWithModifier` | libSceGnmDriver | NATIVE | 716360 |
| 11 | `scePthreadCondBroadcast` | libkernel | NATIVE | 431344 |
| 12 | `pthread_mutex_unlock` | libScePosix | NATIVE | 411005 |
| 13 | `scePthreadCondWait` | libkernel | NATIVE | 268831 |
| 14 | `pthread_mutex_lock` | libScePosix | NATIVE | 265179 |
| 15 | `sceGnmDrawIndexAuto` | libSceGnmDriver | NATIVE | 262128 |
| 16 | `pthread_mutex_trylock` | libScePosix | NATIVE | 149050 |
| 17 | `scePthreadSelf` | libkernel | NATIVE | 131861 |
| 18 | `scePthreadEqual` | libkernel | NATIVE | 71729 |
| 19 | `sceKernelWaitSema` | libkernel | NATIVE | 25732 |
| 20 | `sceKernelClockGettime` | libkernel | NATIVE | 14491 |
| 21 | `sceGnmDingDong` | libSceGnmDriver | NATIVE | 14249 |
| 22 | `sceKernelSignalSema` | libkernel | NATIVE | 8740 |
| 23 | `memset` | libSceLibcInternal | NATIVE | 6903 |
| 24 | `sceGnmDrawInitDefaultHardwareState350` | libSceGnmDriver | NATIVE | 6594 |
| 25 | `__tls_get_addr` | libkernel | NATIVE | 6469 |
| 26 | `sceAudioOutOutputs` | libSceAudioOut | NATIVE | 5504 |
| 27 | `sceGnmDrawIndirect` | libSceGnmDriver | NATIVE | 5120 |
| 28 | `__error` | libkernel | NATIVE | 4108 |
| 29 | `sceGnmSubmitDone` | libSceGnmDriver | NATIVE | 3422 |
| 30 | `sceKernelSetEventFlag` | libkernel | NATIVE | 3320 |
| 31 | `sceGnmInsertWaitFlipDone` | libSceGnmDriver | NATIVE | 3297 |
| 32 | `scePadReadState` | libScePad | NATIVE | 3297 |
| 33 | `scePadSetLightBar` | libScePad | NATIVE | 3297 |
| 34 | `sceUserServiceGetEvent` | libSceUserService | NATIVE | 3297 |
| 35 | `sceNpCheckCallback` | libSceNpManager | NATIVE | 3296 |
| 36 | `scePadSetVibration` | libScePad | NATIVE | 3296 |
| 37 | `sceSystemServiceGetStatus` | libSceSystemService | NATIVE | 3296 |
| 38 | `sceVideoOutIsFlipPending` | libSceVideoOut | NATIVE | 3296 |
| 39 | `sceGnmSubmitAndFlipCommandBuffers` | libSceGnmDriver | NATIVE | 3295 |
| 40 | `sceVideodecDecode` | libSceVideodec | NATIVE | 2749 |
| 41 | `scePthreadRwlockUnlock` | libkernel | NATIVE | 2305 |
| 42 | `scePthreadRwlockRdlock` | libkernel | NATIVE | 2304 |
| 43 | `sceDiscMapIsRequestOnHDD` | libSceDiscMap | NATIVE | 2229 |
| 44 | `?` | libSceDiscMap | NATIVE | 2192 |
| 45 | `sceKernelPread` | libkernel | NATIVE | 2053 |
| 46 | `clock_gettime` | libScePosix | NATIVE | 1071 |
| 47 | `scePthreadMutexInit` | libkernel | NATIVE | 157 |
| 48 | `scePthreadMutexattrDestroy` | libkernel | NATIVE | 144 |
| 49 | `scePthreadMutexattrInit` | libkernel | NATIVE | 144 |
| 50 | `scePthreadMutexattrSettype` | libkernel | NATIVE | 143 |
| 51 | `sceKernelUsleep` | libkernel | NATIVE | 138 |
| 52 | `sceKernelStat` | libkernel | NATIVE | 109 |
| 53 | `scePlayGoGetProgress` | libScePlayGo | NATIVE | 71 |
| 54 | `sceKernelOpen` | libkernel | NATIVE | 66 |
| 55 | `sceKernelClose` | libkernel | NATIVE | 56 |
| 56 | `sceKernelWaitEventFlag` | libkernel | NATIVE | 25 |
| 57 | `scePthreadCondInit` | libkernel | NATIVE | 23 |
| 58 | `sceKernelCreateSema` | libkernel | NATIVE | 20 |
| 59 | `scePthreadAttrDestroy` | libkernel | NATIVE | 20 |
| 60 | `scePthreadAttrInit` | libkernel | NATIVE | 20 |
| 61 | `scePthreadAttrSetdetachstate` | libkernel | NATIVE | 19 |
| 62 | `scePthreadAttrSetinheritsched` | libkernel | NATIVE | 19 |
| 63 | `scePthreadAttrSetschedparam` | libkernel | NATIVE | 19 |
| 64 | `scePthreadAttrSetstacksize` | libkernel | NATIVE | 19 |
| 65 | `scePthreadCreate` | libkernel | NATIVE | 19 |
| 66 | `sceAppContentGetAddcontInfo` | libSceAppContent | NATIVE | 16 |
| 67 | `sceKernelCreateEventFlag` | libkernel | NATIVE | 15 |
| 68 | `sceKernelDeleteEventFlag` | libkernel | NATIVE | 12 |
| 69 | `pthread_mutex_init` | libScePosix | NATIVE | 11 |
| 70 | `pthread_mutexattr_destroy` | libScePosix | NATIVE | 11 |
| 71 | `pthread_mutexattr_init` | libScePosix | NATIVE | 11 |
| 72 | `pthread_mutexattr_settype` | libScePosix | NATIVE | 11 |
| 73 | `scePthreadMutexattrSetprotocol` | libkernel | NATIVE | 10 |
| 74 | `sceSysmoduleLoadModule` | libSceSysmodule | NATIVE | 9 |
| 75 | `scePthreadMutexDestroy` | libkernel | NATIVE | 9 |
| 76 | `sceKernelFstat` | libkernel | NATIVE | 7 |
| 77 | `sceKernelGetDirectMemorySize` | libkernel | NATIVE | 4 |
| 78 | `sceAudioOutInit` | libSceAudioOut | NATIVE | 3 |
| 79 | `sceSaveDataDirNameSearch` | libSceSaveData | NATIVE | 3 |
| 80 | `sceVideodecFlush` | libSceVideodec | NATIVE | 3 |
| 81 | `sceKernelAllocateDirectMemory` | libkernel | NATIVE | 3 |
| 82 | `sceKernelMapDirectMemory` | libkernel | NATIVE | 3 |
| 83 | `sceKernelGetProcParam` | libkernel | NATIVE | 3 |
| 84 | `sceKernelLseek` | libkernel | NATIVE | 3 |
| 85 | `sceKernelRead` | libkernel | NATIVE | 3 |
| 86 | `scePthreadRwlockInit` | libkernel | NATIVE | 3 |
| 87 | `sceAudioOutOpen` | libSceAudioOut | NATIVE | 2 |
| 88 | `sceAudioOutSetVolume` | libSceAudioOut | NATIVE | 2 |
| 89 | `sceNetCtlGetInfo` | libSceNetCtl | NATIVE | 2 |
| 90 | `scePlayGoGetLocus` | libScePlayGo | NATIVE | 2 |
| 91 | `sceSaveDataMount2` | libSceSaveData | NATIVE | 2 |
| 92 | `sceSaveDataUmount` | libSceSaveData | NATIVE | 2 |
| 93 | `sceSystemServiceParamGetInt` | libSceSystemService | NATIVE | 2 |
| 94 | `sceVideoOutRegisterBuffers` | libSceVideoOut | NATIVE | 2 |
| 95 | `sceVideodecCreateDecoder` | libSceVideodec | NATIVE | 2 |
| 96 | `sceVideodecQueryResourceInfo` | libSceVideodec | NATIVE | 2 |
| 97 | `sceVideodecReset` | libSceVideodec | NATIVE | 2 |
| 98 | `sceKernelIsNeoMode` | libkernel | NATIVE | 2 |
| 99 | `sceKernelWaitEqueue` | libkernel | NATIVE | 2 |
| 100 | `scePthreadAttrGetschedparam` | libkernel | NATIVE | 2 |
| 101 | `scePthreadAttrSetaffinity` | libkernel | NATIVE | 2 |
| 102 | `scePthreadAttrSetschedpolicy` | libkernel | NATIVE | 2 |
| 103 | `scePthreadCondDestroy` | libkernel | NATIVE | 2 |
| 104 | `scePthreadJoin` | libkernel | NATIVE | 2 |
| 105 | `sceKernelPwrite` | libkernel | NATIVE | 2 |
| 106 | `sceAjmInitialize` | libSceAjm | BRIDGE | 1 |
| 107 | `sceAjmModuleRegister` | libSceAjm | BRIDGE | 1 |
| 108 | `sceAppContentInitialize` | libSceAppContent | NATIVE | 1 |
| 109 | `sceCommonDialogInitialize` | libSceCommonDialog | BRIDGE | 1 |
| 110 | `sceGnmDispatchInitDefaultHardwareState` | libSceGnmDriver | NATIVE | 1 |
| 111 | `sceGnmMapComputeQueue` | libSceGnmDriver | NATIVE | 1 |
| 112 | `sceGnmSubmitCommandBuffers` | libSceGnmDriver | NATIVE | 1 |
| 113 | `sceHttpCreateTemplate` | libSceHttp | NATIVE | 1 |
| 114 | `sceHttpInit` | libSceHttp | NATIVE | 1 |
| 115 | `sceHttpSetConnectTimeOut` | libSceHttp | NATIVE | 1 |
| 116 | `sceHttpSetRecvTimeOut` | libSceHttp | NATIVE | 1 |
| 117 | `sceHttpSetSendTimeOut` | libSceHttp | NATIVE | 1 |
| 118 | `sceHttpsLoadCert` | libSceHttp | NATIVE | 1 |
| 119 | `sceNetInetNtop` | libSceNet | BRIDGE | 1 |
| 120 | `sceNetInit` | libSceNet | BRIDGE | 1 |
| 121 | `sceNetPoolCreate` | libSceNet | BRIDGE | 1 |
| 122 | `sceNetCtlGetState` | libSceNetCtl | NATIVE | 1 |
| 123 | `sceNetCtlInit` | libSceNetCtl | NATIVE | 1 |
| 124 | `sceNetCtlRegisterCallback` | libSceNetCtl | NATIVE | 1 |
| 125 | `sceNpGetState` | libSceNpManager | NATIVE | 1 |
| 126 | `sceNpRegisterGamePresenceCallbackA` | libSceNpManager | NATIVE | 1 |
| 127 | `sceNpRegisterStateCallbackA` | libSceNpManager | NATIVE | 1 |
| 128 | `sceNpSetContentRestriction` | libSceNpManager | NATIVE | 1 |
| 129 | `sceNpSetNpTitleId` | libSceNpManager | NATIVE | 1 |
| 130 | `sceNpTrophyCreateContext` | libSceNpTrophy | NATIVE | 1 |
| 131 | `sceNpTrophyCreateHandle` | libSceNpTrophy | NATIVE | 1 |
| 132 | `sceNpTrophyGetTrophyUnlockState` | libSceNpTrophy | NATIVE | 1 |
| 133 | `sceNpTrophyRegisterContext` | libSceNpTrophy | NATIVE | 1 |
| 134 | `scePadGetControllerInformation` | libScePad | NATIVE | 1 |
| 135 | `scePadInit` | libScePad | NATIVE | 1 |
| 136 | `scePadOpen` | libScePad | NATIVE | 1 |
| 137 | `scePlayGoGetToDoList` | libScePlayGo | NATIVE | 1 |
| 138 | `scePlayGoInitialize` | libScePlayGo | NATIVE | 1 |
| 139 | `scePlayGoOpen` | libScePlayGo | NATIVE | 1 |
| 140 | `scePlayGoSetLanguageMask` | libScePlayGo | NATIVE | 1 |
| 141 | `scePlayGoSetToDoList` | libScePlayGo | NATIVE | 1 |
| 142 | `sceSaveDataInitialize3` | libSceSaveData | NATIVE | 1 |
| 143 | `sceSaveDataSetParam` | libSceSaveData | NATIVE | 1 |
| 144 | `sceSslInit` | libSceSsl | NATIVE | 1 |
| 145 | `sceSystemServiceGetDisplaySafeAreaInfo` | libSceSystemService | NATIVE | 1 |
| 146 | `sceSystemServiceHideSplashScreen` | libSceSystemService | NATIVE | 1 |
| 147 | `sceSystemServiceParamGetString` | libSceSystemService | NATIVE | 1 |
| 148 | `sceUserServiceGetInitialUser` | libSceUserService | NATIVE | 1 |
| 149 | `sceUserServiceGetLoginUserIdList` | libSceUserService | NATIVE | 1 |
| 150 | `sceUserServiceInitialize` | libSceUserService | NATIVE | 1 |
| 151 | `sceVideoOutAdjustColor_` | libSceVideoOut | NATIVE | 1 |
| 152 | `sceVideoOutColorSettingsSetGamma_` | libSceVideoOut | NATIVE | 1 |
| 153 | `sceVideoOutConfigureOutputMode_` | libSceVideoOut | NATIVE | 1 |
| 154 | `sceVideoOutGetDeviceCapabilityInfo_` | libSceVideoOut | NATIVE | 1 |
| 155 | `sceVideoOutModeSetAny_` | libSceVideoOut | NATIVE | 1 |
| 156 | `sceVideoOutOpen` | libSceVideoOut | NATIVE | 1 |
| 157 | `sceVideoOutSetBufferAttribute` | libSceVideoOut | NATIVE | 1 |
| 158 | `sceVideoOutSetFlipRate` | libSceVideoOut | NATIVE | 1 |
| 159 | `sceVideodecDeleteDecoder` | libSceVideodec | NATIVE | 1 |
| 160 | `sceKernelAddUserEvent` | libkernel | NATIVE | 1 |
| 161 | `sceKernelCreateEqueue` | libkernel | NATIVE | 1 |
| 162 | `sceKernelDeleteUserEvent` | libkernel | NATIVE | 1 |
| 163 | `sceKernelGetTscFrequency` | libkernel | NATIVE | 1 |
| 164 | `sceKernelTriggerUserEvent` | libkernel | NATIVE | 1 |
| 165 | `sceLibcHeapGetTraceInfo` | libSceLibcInternalExt | BRIDGE | 1 |
| 166 | `_sceKernelRtldSetApplicationHeapAPI` | libkernel | NATIVE | 1 |
| 167 | `clock_gettime` | libkernel | NATIVE | 1 |
| 168 | `scePthreadAttrGet` | libkernel | NATIVE | 1 |
| 169 | `scePthreadAttrGetaffinity` | libkernel | NATIVE | 1 |
| 170 | `fflush` | libSceLibcInternal | BRIDGE | 1 |
| 171 | `vprintf` | libSceLibcInternal | COMMON_STUB | 1 |
| 172 | `scePthreadRwlockWrlock` | libkernel | NATIVE | 1 |

## Не вызывались в этом прогоне (по модулям)

| Модуль | Функций |
|---|---:|
| libSceAjm | 9 |
| libSceAudioOut | 2 |
| libSceErrorDialog | 4 |
| libSceGnmDriver | 67 |
| libSceHttp | 12 |
| libSceLibcInternal | 13 |
| libSceMsgDialog | 3 |
| libSceNet | 27 |
| libSceNetCtl | 2 |
| libSceNpManager | 8 |
| libSceNpTrophy | 3 |
| libScePad | 1 |
| libScePlayGo | 3 |
| libSceSaveData | 3 |
| libSceSaveDataDialog | 5 |
| libSceSsl | 1 |
| libSceSysmodule | 1 |
| libSceSystemService | 1 |
| libSceUserService | 1 |
| libSceVideoOut | 3 |
| libkernel | 76 |

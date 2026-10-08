# CALLS — реальные вызовы импортов (Phase 0)

Источник: `E:\SecondSonPC\logs\runs\ru_phase0\calls.json` (снимок 2026-10-08 01:22:02). Счётчиком обёрнуто 4622 функций: native 2, observed 2, bridge 4618 (весь HLE-реестр shadPS4).

Прогон: tag=ru_phase0 lang='' seconds=150

## Владельцы 417 функций, которые должен предоставить рантайм

| Владелец | Всего | Вызваны в прогоне |
|---|---:|---:|
| NATIVE | 2 | 2 |
| OBSERVED | 2 | 2 |
| BRIDGE | 392 | 167 |
| COMMON_STUB | 21 | 0 |
| **Итого** | **417** | **171** |
| LLE (модули игры, не входят в 417) | 147 | не считаются |

OBSERVED — наблюдатель SecondSonPC перед реализацией shadPS4; для миграции это всё ещё BRIDGE.
COMMON_STUB-вызовы счётчик не видит (shadPS4 пишет их в `logs\bridge_shadps4.log`: `CommonStub`).

## Вызванные функции (по убыванию)

| # | Функция | Библиотека | Владелец | Вызовов |
|---:|---|---|---|---:|
| 1 | `scePthreadMutexLock` | libkernel | BRIDGE | 12831738 |
| 2 | `scePthreadMutexUnlock` | libkernel | BRIDGE | 12822505 |
| 3 | `sceGnmDrawIndex` | libSceGnmDriver | BRIDGE | 5863663 |
| 4 | `sceGnmUpdateVsShader` | libSceGnmDriver | BRIDGE | 2975030 |
| 5 | `sceGnmUpdatePsShader350` | libSceGnmDriver | BRIDGE | 2647363 |
| 6 | `sceGnmSetPsShader350` | libSceGnmDriver | BRIDGE | 1375034 |
| 7 | `sceGnmSetVsShader` | libSceGnmDriver | BRIDGE | 1047367 |
| 8 | `sceGnmDrawIndexAuto` | libSceGnmDriver | BRIDGE | 914685 |
| 9 | `pthread_mutex_unlock` | libScePosix | BRIDGE | 714213 |
| 10 | `scePthreadCondBroadcast` | libkernel | BRIDGE | 593830 |
| 11 | `scePthreadCondWait` | libkernel | BRIDGE | 564054 |
| 12 | `pthread_mutex_lock` | libScePosix | BRIDGE | 406772 |
| 13 | `sceGnmDispatchDirect` | libSceGnmDriver | BRIDGE | 356846 |
| 14 | `pthread_mutex_trylock` | libScePosix | BRIDGE | 307753 |
| 15 | `sceGnmSetCsShaderWithModifier` | libSceGnmDriver | BRIDGE | 143355 |
| 16 | `scePthreadSelf` | libkernel | BRIDGE | 76089 |
| 17 | `sceKernelWaitSema` | libkernel | BRIDGE | 75760 |
| 18 | `scePthreadEqual` | libkernel | BRIDGE | 41421 |
| 19 | `sceGnmDrawIndexIndirect` | libSceGnmDriver | BRIDGE | 30234 |
| 20 | `sceKernelSignalSema` | libkernel | BRIDGE | 19512 |
| 21 | `sceGnmDrawInitDefaultHardwareState350` | libSceGnmDriver | BRIDGE | 17202 |
| 22 | `scePadReadState` | libScePad | BRIDGE | 17202 |
| 23 | `scePadSetLightBar` | libScePad | BRIDGE | 17202 |
| 24 | `scePadSetVibration` | libScePad | BRIDGE | 17200 |
| 25 | `sceGnmDrawIndirect` | libSceGnmDriver | BRIDGE | 15597 |
| 26 | `sceKernelClockGettime` | libkernel | BRIDGE | 15016 |
| 27 | `sceGnmDingDong` | libSceGnmDriver | BRIDGE | 9092 |
| 28 | `sceGnmSubmitDone` | libSceGnmDriver | BRIDGE | 8647 |
| 29 | `sceKernelSetEventFlag` | libkernel | BRIDGE | 8626 |
| 30 | `sceUserServiceGetEvent` | libSceUserService | BRIDGE | 8602 |
| 31 | `sceGnmInsertWaitFlipDone` | libSceGnmDriver | BRIDGE | 8601 |
| 32 | `sceNpCheckCallback` | libSceNpManager | BRIDGE | 8600 |
| 33 | `sceSystemServiceGetStatus` | libSceSystemService | BRIDGE | 8600 |
| 34 | `sceVideoOutIsFlipPending` | libSceVideoOut | BRIDGE | 8600 |
| 35 | `sceGnmSubmitAndFlipCommandBuffers` | libSceGnmDriver | BRIDGE | 8599 |
| 36 | `sceAudioOutOutputs` | libSceAudioOut | BRIDGE | 6964 |
| 37 | `memset` | libSceLibcInternal | BRIDGE | 4074 |
| 38 | `__tls_get_addr` | libkernel | BRIDGE | 3653 |
| 39 | `scePthreadRwlockUnlock` | libkernel | BRIDGE | 1353 |
| 40 | `scePthreadRwlockRdlock` | libkernel | BRIDGE | 1352 |
| 41 | `sceDiscMapIsRequestOnHDD` | libSceDiscMap | BRIDGE | 1290 |
| 42 | `?` | libSceDiscMap | BRIDGE | 1252 |
| 43 | `sceKernelPread` | libkernel | BRIDGE | 1137 |
| 44 | `clock_gettime` | libScePosix | BRIDGE | 1090 |
| 45 | `sceVideodecDecode` | libSceVideodec | BRIDGE | 308 |
| 46 | `scePthreadMutexInit` | libkernel | BRIDGE | 185 |
| 47 | `scePthreadMutexattrDestroy` | libkernel | BRIDGE | 174 |
| 48 | `scePthreadMutexattrInit` | libkernel | BRIDGE | 174 |
| 49 | `scePthreadMutexattrSettype` | libkernel | BRIDGE | 173 |
| 50 | `sceKernelStat` | libkernel | OBSERVED | 96 |
| 51 | `scePlayGoGetProgress` | libScePlayGo | BRIDGE | 71 |
| 52 | `sceKernelUsleep` | libkernel | BRIDGE | 60 |
| 53 | `sceKernelOpen` | libkernel | OBSERVED | 53 |
| 54 | `sceKernelClose` | libkernel | BRIDGE | 44 |
| 55 | `sceKernelWaitEventFlag` | libkernel | BRIDGE | 27 |
| 56 | `scePthreadAttrDestroy` | libkernel | BRIDGE | 21 |
| 57 | `scePthreadAttrInit` | libkernel | BRIDGE | 21 |
| 58 | `scePthreadCondInit` | libkernel | BRIDGE | 21 |
| 59 | `sceKernelCreateSema` | libkernel | BRIDGE | 20 |
| 60 | `scePthreadAttrSetdetachstate` | libkernel | BRIDGE | 20 |
| 61 | `scePthreadAttrSetinheritsched` | libkernel | BRIDGE | 20 |
| 62 | `scePthreadAttrSetschedparam` | libkernel | BRIDGE | 20 |
| 63 | `scePthreadAttrSetstacksize` | libkernel | BRIDGE | 20 |
| 64 | `scePthreadCreate` | libkernel | BRIDGE | 20 |
| 65 | `sceAppContentGetAddcontInfo` | libSceAppContent | BRIDGE | 16 |
| 66 | `sceKernelCreateEventFlag` | libkernel | BRIDGE | 16 |
| 67 | `sceKernelDeleteEventFlag` | libkernel | BRIDGE | 13 |
| 68 | `pthread_mutex_init` | libScePosix | BRIDGE | 11 |
| 69 | `pthread_mutexattr_destroy` | libScePosix | BRIDGE | 11 |
| 70 | `pthread_mutexattr_init` | libScePosix | BRIDGE | 11 |
| 71 | `pthread_mutexattr_settype` | libScePosix | BRIDGE | 11 |
| 72 | `scePthreadMutexDestroy` | libkernel | BRIDGE | 10 |
| 73 | `scePthreadMutexattrSetprotocol` | libkernel | BRIDGE | 10 |
| 74 | `sceSysmoduleLoadModule` | libSceSysmodule | BRIDGE | 9 |
| 75 | `__error` | libkernel | BRIDGE | 8 |
| 76 | `sceKernelFstat` | libkernel | BRIDGE | 7 |
| 77 | `sceAudioOutInit` | libSceAudioOut | BRIDGE | 4 |
| 78 | `sceSaveDataDirNameSearch` | libSceSaveData | BRIDGE | 4 |
| 79 | `sceKernelGetDirectMemorySize` | libkernel | BRIDGE | 4 |
| 80 | `sceKernelPwrite` | libkernel | BRIDGE | 4 |
| 81 | `sceAudioOutOpen` | libSceAudioOut | BRIDGE | 3 |
| 82 | `sceAudioOutSetVolume` | libSceAudioOut | BRIDGE | 3 |
| 83 | `sceSaveDataMount2` | libSceSaveData | BRIDGE | 3 |
| 84 | `sceSaveDataUmount` | libSceSaveData | BRIDGE | 3 |
| 85 | `sceVideodecFlush` | libSceVideodec | BRIDGE | 3 |
| 86 | `sceKernelAllocateDirectMemory` | libkernel | BRIDGE | 3 |
| 87 | `sceKernelMapDirectMemory` | libkernel | BRIDGE | 3 |
| 88 | `scePthreadJoin` | libkernel | BRIDGE | 3 |
| 89 | `sceKernelGetProcParam` | libkernel | BRIDGE | 3 |
| 90 | `sceKernelLseek` | libkernel | BRIDGE | 3 |
| 91 | `sceKernelRead` | libkernel | BRIDGE | 3 |
| 92 | `scePthreadRwlockInit` | libkernel | BRIDGE | 3 |
| 93 | `sceNetCtlGetInfo` | libSceNetCtl | BRIDGE | 2 |
| 94 | `scePadGetControllerInformation` | libScePad | BRIDGE | 2 |
| 95 | `scePadOpen` | libScePad | BRIDGE | 2 |
| 96 | `scePlayGoGetLocus` | libScePlayGo | BRIDGE | 2 |
| 97 | `sceSaveDataSetParam` | libSceSaveData | BRIDGE | 2 |
| 98 | `sceSystemServiceParamGetInt` | libSceSystemService | NATIVE | 2 |
| 99 | `sceVideoOutRegisterBuffers` | libSceVideoOut | BRIDGE | 2 |
| 100 | `sceKernelIsNeoMode` | libkernel | BRIDGE | 2 |
| 101 | `sceKernelWaitEqueue` | libkernel | BRIDGE | 2 |
| 102 | `scePthreadAttrGetschedparam` | libkernel | BRIDGE | 2 |
| 103 | `scePthreadAttrSetaffinity` | libkernel | BRIDGE | 2 |
| 104 | `scePthreadAttrSetschedpolicy` | libkernel | BRIDGE | 2 |
| 105 | `scePthreadCondDestroy` | libkernel | BRIDGE | 2 |
| 106 | `sceAjmInitialize` | libSceAjm | BRIDGE | 1 |
| 107 | `sceAjmModuleRegister` | libSceAjm | BRIDGE | 1 |
| 108 | `sceAppContentInitialize` | libSceAppContent | BRIDGE | 1 |
| 109 | `sceCommonDialogInitialize` | libSceCommonDialog | BRIDGE | 1 |
| 110 | `sceGnmDispatchInitDefaultHardwareState` | libSceGnmDriver | BRIDGE | 1 |
| 111 | `sceGnmMapComputeQueue` | libSceGnmDriver | BRIDGE | 1 |
| 112 | `sceGnmSubmitCommandBuffers` | libSceGnmDriver | BRIDGE | 1 |
| 113 | `sceHttpCreateTemplate` | libSceHttp | BRIDGE | 1 |
| 114 | `sceHttpInit` | libSceHttp | BRIDGE | 1 |
| 115 | `sceHttpSetConnectTimeOut` | libSceHttp | BRIDGE | 1 |
| 116 | `sceHttpSetRecvTimeOut` | libSceHttp | BRIDGE | 1 |
| 117 | `sceHttpSetSendTimeOut` | libSceHttp | BRIDGE | 1 |
| 118 | `sceHttpsLoadCert` | libSceHttp | BRIDGE | 1 |
| 119 | `sceNetInetNtop` | libSceNet | BRIDGE | 1 |
| 120 | `sceNetInit` | libSceNet | BRIDGE | 1 |
| 121 | `sceNetPoolCreate` | libSceNet | BRIDGE | 1 |
| 122 | `sceNetCtlGetState` | libSceNetCtl | BRIDGE | 1 |
| 123 | `sceNetCtlInit` | libSceNetCtl | BRIDGE | 1 |
| 124 | `sceNetCtlRegisterCallback` | libSceNetCtl | BRIDGE | 1 |
| 125 | `sceNpGetState` | libSceNpManager | BRIDGE | 1 |
| 126 | `sceNpRegisterGamePresenceCallbackA` | libSceNpManager | BRIDGE | 1 |
| 127 | `sceNpRegisterStateCallbackA` | libSceNpManager | BRIDGE | 1 |
| 128 | `sceNpSetContentRestriction` | libSceNpManager | BRIDGE | 1 |
| 129 | `sceNpSetNpTitleId` | libSceNpManager | BRIDGE | 1 |
| 130 | `sceNpTrophyCreateContext` | libSceNpTrophy | BRIDGE | 1 |
| 131 | `sceNpTrophyCreateHandle` | libSceNpTrophy | BRIDGE | 1 |
| 132 | `sceNpTrophyGetTrophyUnlockState` | libSceNpTrophy | BRIDGE | 1 |
| 133 | `sceNpTrophyRegisterContext` | libSceNpTrophy | BRIDGE | 1 |
| 134 | `scePadInit` | libScePad | BRIDGE | 1 |
| 135 | `scePlayGoGetToDoList` | libScePlayGo | BRIDGE | 1 |
| 136 | `scePlayGoInitialize` | libScePlayGo | BRIDGE | 1 |
| 137 | `scePlayGoOpen` | libScePlayGo | BRIDGE | 1 |
| 138 | `scePlayGoSetLanguageMask` | libScePlayGo | BRIDGE | 1 |
| 139 | `scePlayGoSetToDoList` | libScePlayGo | BRIDGE | 1 |
| 140 | `sceSaveDataInitialize3` | libSceSaveData | BRIDGE | 1 |
| 141 | `sceSslInit` | libSceSsl | BRIDGE | 1 |
| 142 | `sceSystemServiceGetDisplaySafeAreaInfo` | libSceSystemService | BRIDGE | 1 |
| 143 | `sceSystemServiceHideSplashScreen` | libSceSystemService | BRIDGE | 1 |
| 144 | `sceSystemServiceParamGetString` | libSceSystemService | NATIVE | 1 |
| 145 | `sceUserServiceGetInitialUser` | libSceUserService | BRIDGE | 1 |
| 146 | `sceUserServiceGetLoginUserIdList` | libSceUserService | BRIDGE | 1 |
| 147 | `sceUserServiceInitialize` | libSceUserService | BRIDGE | 1 |
| 148 | `sceVideoOutAdjustColor_` | libSceVideoOut | BRIDGE | 1 |
| 149 | `sceVideoOutColorSettingsSetGamma_` | libSceVideoOut | BRIDGE | 1 |
| 150 | `sceVideoOutConfigureOutputMode_` | libSceVideoOut | BRIDGE | 1 |
| 151 | `sceVideoOutGetDeviceCapabilityInfo_` | libSceVideoOut | BRIDGE | 1 |
| 152 | `sceVideoOutModeSetAny_` | libSceVideoOut | BRIDGE | 1 |
| 153 | `sceVideoOutOpen` | libSceVideoOut | BRIDGE | 1 |
| 154 | `sceVideoOutSetBufferAttribute` | libSceVideoOut | BRIDGE | 1 |
| 155 | `sceVideoOutSetFlipRate` | libSceVideoOut | BRIDGE | 1 |
| 156 | `sceVideodecCreateDecoder` | libSceVideodec | BRIDGE | 1 |
| 157 | `sceVideodecDeleteDecoder` | libSceVideodec | BRIDGE | 1 |
| 158 | `sceVideodecQueryResourceInfo` | libSceVideodec | BRIDGE | 1 |
| 159 | `sceVideodecReset` | libSceVideodec | BRIDGE | 1 |
| 160 | `sceKernelAddUserEvent` | libkernel | BRIDGE | 1 |
| 161 | `sceKernelCreateEqueue` | libkernel | BRIDGE | 1 |
| 162 | `sceKernelDeleteUserEvent` | libkernel | BRIDGE | 1 |
| 163 | `sceKernelGetTscFrequency` | libkernel | BRIDGE | 1 |
| 164 | `sceKernelTriggerUserEvent` | libkernel | BRIDGE | 1 |
| 165 | `sceLibcHeapGetTraceInfo` | libSceLibcInternalExt | BRIDGE | 1 |
| 166 | `_sceKernelRtldSetApplicationHeapAPI` | libkernel | BRIDGE | 1 |
| 167 | `clock_gettime` | libkernel | BRIDGE | 1 |
| 168 | `scePthreadAttrGet` | libkernel | BRIDGE | 1 |
| 169 | `scePthreadAttrGetaffinity` | libkernel | BRIDGE | 1 |
| 170 | `fflush` | libSceLibcInternal | BRIDGE | 1 |
| 171 | `scePthreadRwlockWrlock` | libkernel | BRIDGE | 1 |

## Не вызывались в этом прогоне (по модулям)

| Модуль | Функций |
|---|---:|
| libSceAjm | 9 |
| libSceAudioOut | 2 |
| libSceErrorDialog | 4 |
| libSceGnmDriver | 67 |
| libSceHttp | 12 |
| libSceLibcInternal | 14 |
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

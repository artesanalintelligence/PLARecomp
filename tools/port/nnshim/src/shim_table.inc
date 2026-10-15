/* nn shim table: which SDK function each native adapter replaces.
 *
 * SHIM(site, status, "mangled symbol", adapter)
 *   site    SDK  = hook the DEFINITION in the sdk module (catches every caller:
 *                  main, the SDK's musl and the SDK itself);
 *           PLT  = hook main's PLT stub (only calls from main);
 *           SDKX = definition in sdk of a function main does NOT import (SDK internals,
 *                  e.g. hid::detail::EnqueueVibrationValue); its offset comes from
 *                  generated/sdk_symbols.inc (gen_sdk_symbols.py).
 *   status  NATIVE = implemented here (the adapter returns PLA_SKIP, or PLA_CONTINUE if
 *                    the resource is not the shim's: foreign mount/handle);
 *           PASS   = deliberately left to the original SDK (suyu's HLE in v1);
 *           TODO   = pending.
 * Offsets come from generated/plt_imports.inc (tools/port/inventory.py): gen_mod_json.py
 * joins both tables and writes mod.json. PASS/TODO entries are not hooked.
 * Each row's family is the inventory's (column 1 of plt_imports.inc): hid, fs, time,
 * settings, applet (oe), account, err, swkbd, font (pl).
 *
 * Why the SDK site almost everywhere: the SDK itself calls these functions through its
 * internal PLT (e.g. time from account, err::ShowError from other libraries); hooking the
 * definition catches them all. hid reads use PLT (only the game calls them, and that way
 * the already-tested prototype does not change).
 */

/* ---- nn::hid: input ------------------------------------------------------------------ */
SHIM(PLT, NATIVE, "_ZN2nn3hid15GetNpadStyleSetERKj",                         nnshim_hid_GetNpadStyleSet)
SHIM(PLT, NATIVE, "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",    nnshim_hid_GetNpadState_FullKey)
SHIM(PLT, NATIVE, "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",   nnshim_hid_GetNpadState_Handheld)
SHIM(PLT, NATIVE, "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",    nnshim_hid_GetNpadState_JoyDual)
SHIM(PLT, NATIVE, "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",    nnshim_hid_GetNpadState_JoyLeft)
SHIM(PLT, NATIVE, "_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",   nnshim_hid_GetNpadState_JoyRight)
/* Controller applet setup: the HLE stays consistent. The style-change event is
 * native (pla_api 1.1: runtime KEvent). */
SHIM(PLT, PASS,   "_ZN2nn3hid14InitializeNpadEv",                                                   none)
SHIM(PLT, PASS,   "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", none)
SHIM(PLT, PASS,   "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",                                        none)
SHIM(PLT, PASS,   "_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",                          none)
SHIM(PLT, PASS,   "_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE",   none)
SHIM(PLT, NATIVE, "_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", nnshim_hid_BindNpadStyleSetUpdateEvent)

/* ---- nn::hid: motion (12) -> SDL2 gyroscope/accelerometer ------------------------------ */
SHIM(SDK, NATIVE, "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", nnshim_hid_GetSixAxisSensorHandles)
SHIM(SDK, NATIVE, "_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE",  nnshim_hid_StartSixAxisSensor)
SHIM(SDK, NATIVE, "_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE",   nnshim_hid_StopSixAxisSensor)
SHIM(SDK, NATIVE, "_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", nnshim_hid_GetSixAxisSensorState)
SHIM(SDK, NATIVE, "_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", nnshim_hid_GetSixAxisSensorStates)
SHIM(SDK, NATIVE, "_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", nnshim_hid_IsSixAxisSensorAtRest)
SHIM(SDK, NATIVE, "_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", nnshim_hid_EnableSixAxisSensorFusion)
SHIM(SDK, NATIVE, "_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE", nnshim_hid_IsSixAxisSensorFusionEnabled)
SHIM(SDK, NATIVE, "_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", nnshim_hid_SetSixAxisSensorFusionParameters)
SHIM(SDK, NATIVE, "_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE", nnshim_hid_GetSixAxisSensorFusionParameters)
SHIM(SDK, NATIVE, "_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", nnshim_hid_SetGyroscopeZeroDriftMode)
SHIM(SDK, NATIVE, "_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", nnshim_hid_GetGyroscopeZeroDriftMode)

/* ---- nn::hid: vibration -> SDL_GameControllerRumble / XInputSetState ------------------- */
/* Devices (handles and info) are native; the player/mixer/nodes are SDK user code
 * (they stay) and end up in the internal functions below. */
SHIM(SDK,  NATIVE, "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", nnshim_hid_GetVibrationDeviceHandles)
SHIM(SDK,  NATIVE, "_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", nnshim_hid_InitializeVibrationDevice)
SHIM(SDKX, NATIVE, "_ZN2nn3hid6detail21EnqueueVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", nnshim_hid_EnqueueVibrationValue)
SHIM(SDKX, NATIVE, "_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", nnshim_hid_SendVibrationValue)
SHIM(SDKX, NATIVE, "_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", nnshim_hid_SendVibrationValues)
SHIM(SDKX, NATIVE, "_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", nnshim_hid_GetVibrationDeviceInfo)

/* ---- nn::fs ------------------------------------------------------------------------ */
/* SDK site: the SDK's musl (fopen) and the SDK itself also open "rom:/" paths. */
SHIM(SDK, NATIVE, "_ZN2nn2fs8MountRomEPKc",                                   nnshim_fs_MountRom)
SHIM(SDK, NATIVE, "_ZN2nn2fs8MountRomEPKcPvm",                                nnshim_fs_MountRomCache)
SHIM(SDK, NATIVE, "_ZN2nn2fs22QueryMountRomCacheSizeEPm",                     nnshim_fs_QueryMountRomCacheSize)
SHIM(SDK, NATIVE, "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE",           nnshim_fs_MountSaveData)
SHIM(SDK, NATIVE, "_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", nnshim_fs_MountSaveDataReadOnly)
SHIM(SDK, NATIVE, "_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE",             nnshim_fs_EnsureSaveData)
SHIM(SDK, NATIVE, "_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE",         nnshim_fs_IsSaveDataExisting)
SHIM(SDK, NATIVE, "_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", nnshim_fs_IsSaveDataExistingApp)
SHIM(SDK, NATIVE, "_ZN2nn2fs7UnmountEPKc",                                    nnshim_fs_Unmount)
SHIM(SDK, NATIVE, "_ZN2nn2fs6CommitEPKc",                                     nnshim_fs_Commit)
SHIM(SDK, NATIVE, "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",                nnshim_fs_OpenFile)
SHIM(SDK, NATIVE, "_ZN2nn2fs9CloseFileENS0_10FileHandleE",                    nnshim_fs_CloseFile)
SHIM(SDK, NATIVE, "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",               nnshim_fs_ReadFileOut)
SHIM(SDK, NATIVE, "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm",                 nnshim_fs_ReadFile)
SHIM(SDK, NATIVE, "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", nnshim_fs_WriteFile)
SHIM(SDK, NATIVE, "_ZN2nn2fs9FlushFileENS0_10FileHandleE",                    nnshim_fs_FlushFile)
SHIM(SDK, NATIVE, "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",               nnshim_fs_GetFileSize)
SHIM(SDK, NATIVE, "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl",                nnshim_fs_SetFileSize)
SHIM(SDK, NATIVE, "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc",    nnshim_fs_GetEntryType)
SHIM(SDK, NATIVE, "_ZN2nn2fs10CreateFileEPKcl",                               nnshim_fs_CreateFile)
SHIM(SDK, NATIVE, "_ZN2nn2fs10DeleteFileEPKc",                                nnshim_fs_DeleteFile)
SHIM(SDK, NATIVE, "_ZN2nn2fs15CreateDirectoryEPKc",                           nnshim_fs_CreateDirectory)
SHIM(SDK, NATIVE, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc",                nnshim_fs_DeleteDirectoryRecursively)
SHIM(SDK, NATIVE, "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci",     nnshim_fs_OpenDirectory)
SHIM(SDK, NATIVE, "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", nnshim_fs_ReadDirectory)
SHIM(SDK, NATIVE, "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE",         nnshim_fs_CloseDirectory)
/* Per-thread error context and SDK allocator: they don't affect the shim's mounts. */
SHIM(SDK, PASS,   "_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE",                   none)
SHIM(SDK, PASS,   "_ZN2nn2fs25GetCurrentThreadFsContextEv",                   none)
SHIM(SDK, PASS,   "_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE",   none)

/* ---- nn::time (12) -------------------------------------------------------------------- */
SHIM(SDK, NATIVE, "_ZN2nn4time10InitializeEv",                                nnshim_time_Initialize)
SHIM(SDK, NATIVE, "_ZN2nn4time13IsInitializedEv",                             nnshim_time_IsInitialized)
SHIM(SDK, NATIVE, "_ZN2nn4time8FinalizeEv",                                   nnshim_time_Finalize)
SHIM(SDK, NATIVE, "_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", nnshim_time_UserGetCurrentTime)
SHIM(SDK, NATIVE, "_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", nnshim_time_NetworkGetCurrentTime)
SHIM(SDK, NATIVE, "_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", nnshim_time_ToCalendarTime)
SHIM(SDK, NATIVE, "_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE",       nnshim_time_ToCalendarTimeInUtc)
SHIM(SDK, NATIVE, "_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", nnshim_time_ToPosixTime)
SHIM(SDK, NATIVE, "_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", nnshim_time_LoadTimeZoneRule)
SHIM(SDK, NATIVE, "_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE",  nnshim_time_GetDeviceLocationName)
SHIM(SDK, NATIVE, "_ZN2nn4time13ClockSnapshotC1Ev",                           nnshim_time_ClockSnapshotCtor)
SHIM(SDK, NATIVE, "_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", nnshim_time_CreateWithStandardSystemClock)

/* ---- nn::settings (3) ----------------------------------------------------------------- */
SHIM(SDK, NATIVE, "_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE",        nnshim_settings_LanguageCodeMake)
SHIM(SDK, NATIVE, "_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE",  nnshim_settings_LanguageEqCode)
SHIM(SDK, NATIVE, "_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE",  nnshim_settings_CodeEqLanguage)

/* ---- nn::oe (19) ---------------------------------------------------------------------- */
SHIM(SDK, NATIVE, "_ZN2nn2oe10InitializeEv",                                  nnshim_oe_Initialize)
SHIM(SDK, NATIVE, "_ZN2nn2oe18GetDesiredLanguageEv",                          nnshim_oe_GetDesiredLanguage)
SHIM(SDK, NATIVE, "_ZN2nn2oe16GetOperationModeEv",                            nnshim_oe_GetOperationMode)
SHIM(SDK, NATIVE, "_ZN2nn2oe18GetPerformanceModeEv",                          nnshim_oe_GetPerformanceMode)
SHIM(SDK, NATIVE, "_ZN2nn2oe20GetCurrentFocusStateEv",                        nnshim_oe_GetCurrentFocusState)
SHIM(SDK, NATIVE, "_ZN2nn2oe25TryPopNotificationMessageEPj",                  nnshim_oe_TryPopNotificationMessage)
SHIM(SDK, NATIVE, "_ZN2nn2oe15EnableRecordingEv",                             nnshim_oe_EnableRecording)
SHIM(SDK, NATIVE, "_ZN2nn2oe16DisableRecordingEv",                            nnshim_oe_DisableRecording)
SHIM(SDK, NATIVE, "_ZN2nn2oe16EnableScreenShotEv",                            nnshim_oe_EnableScreenShot)
SHIM(SDK, NATIVE, "_ZN2nn2oe17DisableScreenShotEv",                           nnshim_oe_DisableScreenShot)
SHIM(SDK, NATIVE, "_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE",           nnshim_oe_SetCpuBoostMode)
SHIM(SDK, NATIVE, "_ZN2nn2oe17FinishStartupLogoEv",                           nnshim_oe_FinishStartupLogo)
SHIM(SDK, NATIVE, "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", nnshim_oe_SetFocusHandlingMode)
SHIM(SDK, NATIVE, "_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", nnshim_oe_SetPerformanceConfiguration)
SHIM(SDK, NATIVE, "_ZN2nn2oe31EnterExitRequestHandlingSectionEv",             nnshim_oe_EnterExitRequestHandlingSection)
SHIM(SDK, NATIVE, "_ZN2nn2oe31LeaveExitRequestHandlingSectionEv",             nnshim_oe_LeaveExitRequestHandlingSection)
SHIM(SDK, NATIVE, "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb",      nnshim_oe_SetUserInactivityDetectionTimeExtended)
SHIM(SDK, NATIVE, "_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb",  nnshim_oe_SetOperationModeChangedNotificationEnabled)
SHIM(SDK, NATIVE, "_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", nnshim_oe_SetPerformanceModeChangedNotificationEnabled)

/* ---- nn::account (24: 17 native, 7 PASS) ---------------------------------------------- */
SHIM(SDK, NATIVE, "_ZN2nn7account10InitializeEv",                             nnshim_account_Initialize)
SHIM(SDK, NATIVE, "_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", nnshim_account_TryOpenPreselectedUser)
SHIM(SDK, NATIVE, "_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE",   nnshim_account_GetUserId)
SHIM(SDK, NATIVE, "_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE",   nnshim_account_GetNickname)
SHIM(SDK, NATIVE, "_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE",           nnshim_account_GetUserExistence)
SHIM(SDK, NATIVE, "_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", nnshim_account_IsNetworkServiceAccountAvailable)
SHIM(SDK, NATIVE, "_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", nnshim_account_EnsureNetworkServiceAccountAvailable)
SHIM(SDK, NATIVE, "_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", nnshim_account_ShowLicenseRequirementsForNetworkService)
SHIM(SDK, NATIVE, "_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", nnshim_account_GetNetworkServiceAccountId)
SHIM(SDK, NATIVE, "_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", nnshim_account_LoadNetworkServiceAccountIdTokenCache)
SHIM(SDK, NATIVE, "_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", nnshim_account_EnsureNetworkServiceAccountIdTokenCacheAsync)
SHIM(SDK, NATIVE, "_ZN2nn7account34LoadNetworkServiceLicenseInfoAsyncEPNS0_37AsyncNetworkServiceLicenseInfoContextERKNS0_10UserHandleE", nnshim_account_LoadNetworkServiceLicenseInfoAsync)
SHIM(SDK, NATIVE, "_ZN2nn7account12AsyncContext7HasDoneEPb",                  nnshim_account_AsyncContextHasDone)
SHIM(SDK, NATIVE, "_ZN2nn7account12AsyncContext9GetResultEv",                 nnshim_account_AsyncContextGetResult)
SHIM(SDK, NATIVE, "_ZN2nn7account12AsyncContext6CancelEv",                    nnshim_account_AsyncContextCancel)
SHIM(SDK, NATIVE, "_ZN2nn7account37AsyncNetworkServiceLicenseInfoContext28GetNetworkServiceLicenseKindEv", nnshim_account_GetNetworkServiceLicenseKind)
/* Kernel event: PASS until the pla_api extension (RUNTIME). */
SHIM(SDK, NATIVE, "_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", nnshim_account_AsyncContextGetSystemEvent)
/* SDK user code (no IPC with an empty context). */
SHIM(SDK, PASS,   "_ZN2nn7account12AsyncContextC1Ev",                         none)
SHIM(SDK, PASS,   "_ZN2nn7account12AsyncContextD1Ev",                         none)
SHIM(SDK, PASS,   "_ZN2nn7account12AsyncContextD2Ev",                         none)
SHIM(SDK, PASS,   "_ZN2nn7account12AsyncContextaSEOS1_",                      none)
SHIM(SDK, PASS,   "_ZN2nn7account37AsyncNetworkServiceLicenseInfoContextC1Ev", none)
SHIM(SDK, PASS,   "_ZN2nn7account37AsyncNetworkServiceLicenseInfoContextC1EOS1_", none)
SHIM(SDK, PASS,   "_ZN2nn7account37AsyncNetworkServiceLicenseInfoContextaSEOS1_", none)

/* ---- nn::err / nn::diag (28: 13 native, 15 PASS) --------------------------------------- */
SHIM(SDK, NATIVE, "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i",                  nnshim_diag_AbortImpl)
SHIM(SDK, NATIVE, "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultES3_z", nnshim_diag_AbortImplFormat)
SHIM(SDK, NATIVE, "nndetailDiagAbortFormatImpl",                              nnshim_diag_AbortFormatImplC)
SHIM(SDK, NATIVE, "_ZN2nn6result6detail17OnUnhandledResultENS_6ResultE",     nnshim_result_OnUnhandledResult)
SHIM(SDK, NATIVE, "_ZN2nn4diag12GetBacktraceEPmi",                            nnshim_diag_GetBacktrace)
SHIM(SDK, NATIVE, "_ZN2nn3err19ApplicationErrorArgC1Ev",                      nnshim_err_ApplicationErrorArgCtor)
SHIM(SDK, NATIVE, "_ZN2nn3err19ApplicationErrorArg29SetApplicationErrorCodeNumberEj", nnshim_err_SetApplicationErrorCodeNumber)
SHIM(SDK, NATIVE, "_ZN2nn3err19ApplicationErrorArg16SetDialogMessageEPKc",    nnshim_err_SetDialogMessage)
SHIM(SDK, NATIVE, "_ZN2nn3err19ApplicationErrorArg20SetFullScreenMessageEPKc", nnshim_err_SetFullScreenMessage)
SHIM(SDK, NATIVE, "_ZN2nn3err19ApplicationErrorArg15SetLanguageCodeERKNS_8settings12LanguageCodeE", nnshim_err_SetLanguageCode)
SHIM(SDK, NATIVE, "_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", nnshim_err_ShowApplicationError)
SHIM(SDK, NATIVE, "_ZN2nn3err9ShowErrorENS_6ResultE",                         nnshim_err_ShowErrorResult)
SHIM(SDK, NATIVE, "_ZN2nn3err9ShowErrorENS0_9ErrorCodeE",                     nnshim_err_ShowErrorCode)
SHIM(SDK, NATIVE, "_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE",         nnshim_err_ShowErrorVariant)
/* SDK user code without IPC (stays recompiled). */
SHIM(SDK, PASS,   "_ZN2nn3err18ErrorResultVariantC1ERKNS_6ResultE",           none)
SHIM(SDK, PASS,   "_ZN2nn3err18ErrorResultVariantC1Ev",                       none)
SHIM(SDK, PASS,   "_ZN2nn3err18ErrorResultVariantaSERKNS0_9ErrorCodeE",       none)
SHIM(SDK, PASS,   "_ZN2nn3err18ErrorResultVariantaSERKNS_6ResultE",           none)
SHIM(SDK, PASS,   "_ZNK2nn3err18ErrorResultVariant8GetStateEv",               none)
SHIM(SDK, PASS,   "_ZNK2nn3err18ErrorResultVariantcvNS0_9ErrorCodeEEv",       none)
SHIM(SDK, PASS,   "_ZNK2nn3err18ErrorResultVariantcvNS_6ResultEEv",           none)
SHIM(SDK, PASS,   "_ZNK2nn3err9ErrorCode7IsValidEv",                          none)
SHIM(SDK, PASS,   "nnResultGetDescription",                                   none)
SHIM(SDK, PASS,   "nnResultGetModule",                                        none)
SHIM(SDK, PASS,   "nnResultIsFailure",                                        none)
SHIM(SDK, PASS,   "nnResultIsSuccess",                                        none)
SHIM(SDK, PASS,   "_ZN2nn3err11NplnContext4MakeEPKvm",                        none)
SHIM(SDK, PASS,   "_ZN2nn3err23UpdateResultWithContextINS0_11NplnContextEEENS_6ResultES3_RKT_", none)

/* ---- nn::swkbd (11: 5 native, 6 PASS) -------------------------------------------------- */
SHIM(SDK, NATIVE, "_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", nnshim_swkbd_ShowKeyboard)
SHIM(SDK, NATIVE, "_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb",                nnshim_swkbd_GetRequiredWorkBufferSizeBool)
SHIM(SDK, NATIVE, "_ZN2nn5swkbd25GetRequiredWorkBufferSizeEi",                nnshim_swkbd_GetRequiredWorkBufferSizeInt)
SHIM(SDK, NATIVE, "_ZN2nn5swkbd27GetRequiredStringBufferSizeEv",              nnshim_swkbd_GetRequiredStringBufferSize)
SHIM(SDK, NATIVE, "_ZN2nn5swkbd34GetRequiredTextCheckWorkBufferSizeEv",       nnshim_swkbd_GetRequiredTextCheckWorkBufferSize)
/* They fill KeyboardConfig/ShowKeyboardArg: SDK user code. */
SHIM(SDK, PASS,   "_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE", none)
SHIM(SDK, PASS,   "_ZN2nn5swkbd13SetHeaderTextEPNS0_14KeyboardConfigEPKDs",   none)
SHIM(SDK, PASS,   "_ZN2nn5swkbd10SetSubTextEPNS0_14KeyboardConfigEPKDs",      none)
SHIM(SDK, PASS,   "_ZN2nn5swkbd14SetInitialTextEPNS0_15ShowKeyboardArgEPKDs", none)
SHIM(SDK, PASS,   "_ZN2nn5swkbd15SetUserWordListEPNS0_15ShowKeyboardArgEPKNS0_8UserWordEi", none)
SHIM(SDK, PASS,   "_ZN2nn5swkbd20SetTextCheckCallbackEPNS0_15ShowKeyboardArgEPFNS0_15TextCheckResultEPvPmPNS0_6StringEE", none)

/* ---- nn::pl (4 native) ------------------------------------------------------ */
SHIM(SDK, NATIVE, "_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE",   nnshim_pl_RequestSharedFontLoad)
SHIM(SDK, NATIVE, "_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE",  nnshim_pl_GetSharedFontLoadState)
/* Pointer to guest font memory: native with "font.dir" and pla_api 1.2
 * (guest_alloc); otherwise the adapter hands the call back to the HLE (abi_font.cpp). */
SHIM(SDK, NATIVE, "_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE",    nnshim_pl_GetSharedFontAddress)
SHIM(SDK, NATIVE, "_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE",       nnshim_pl_GetSharedFontSize)

/* ---- offline network/shop (abi_net.cpp; family "net"; v1.5 stage 5) -------------------------- */
SHIM(SDK, NATIVE, "_ZN2nn3aoc17CountAddOnContentEv",                             nnshim_aoc_CountAddOnContent)
SHIM(SDK, NATIVE, "_ZN2nn3aoc16ListAddOnContentEPiii",                           nnshim_aoc_ListAddOnContent)
SHIM(SDK, NATIVE, "_ZN2nn4nifm18IsNetworkAvailableEv",                           nnshim_nifm_IsNetworkAvailable)
SHIM(SDK, NATIVE, "_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", nnshim_friends_GetFriendList)
SHIM(SDK, NATIVE, "_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE",            nnshim_prepo_PlayReportSave)

/* ---- startup services (v1.5 stage 5; abi_os.cpp, family "os"; abi_applet.cpp) -------------------- */
SHIM(SDK,  NATIVE, "_ZN2nn2os19GenerateRandomBytesEPvm",                          nnshim_os_GenerateRandomBytes)
SHIM(SDK,  NATIVE, "_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm",     nnshim_crypto_GenerateCryptographicallyRandomBytes)
SHIM(SDKX, NATIVE, "_ZN2nn6crypto15InitializeCsrngEv",                            nnshim_crypto_InitializeCsrng)
SHIM(SDKX, NATIVE, "_ZN2nn4pctl6detail3ipc28InitializeIpcSessionIfNeededEv",      nnshim_pctl_InitializeIpcSessionIfNeeded)
SHIM(SDKX, NATIVE, "_ZN2nn7account12GetUserCountEPi",                             nnshim_account_GetUserCount)
SHIM(SDKX, NATIVE, "_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi",                 nnshim_account_ListOpenUsers)

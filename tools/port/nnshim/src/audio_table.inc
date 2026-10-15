/* nn::audio. Same format as shim_table.inc; gen_mod_json.py reads this file
 * too. PLT site: only main (the Wwise sink) uses AudioOut.
 * Pure SDK accessors (no IPC) stay in the SDK: the shim keeps their layout. */
SHIM(PLT, NATIVE, "_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", nnshim_audio_OpenDefaultAudioOut)
SHIM(PLT, NATIVE, "_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE",                                  nnshim_audio_StartAudioOut)
SHIM(PLT, NATIVE, "_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE",                                   nnshim_audio_StopAudioOut)
SHIM(PLT, NATIVE, "_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE",                                  nnshim_audio_CloseAudioOut)
SHIM(PLT, NATIVE, "_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE",                              nnshim_audio_GetAudioOutState)
SHIM(PLT, NATIVE, "_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE",     nnshim_audio_AppendAudioOutBuffer)
SHIM(PLT, NATIVE, "_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE",                      nnshim_audio_GetReleasedAudioOutBuffer)
SHIM(PLT, PASS,   "_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE",          none)
SHIM(PLT, PASS,   "_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm",               none)
SHIM(PLT, PASS,   "_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE",           none)
SHIM(PLT, PASS,   "_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE",                       none)
/* Output device and its notification (event): HLE queries, no effect on the sound. */
SHIM(PLT, PASS,   "_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE",               none)
SHIM(PLT, PASS,   "_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv",                    none)
SHIM(PLT, PASS,   "_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE",  none)
/* "Hardware" Opus (v1.5 stage 5): main only imports the constructor; the rest goes through the
 * vtable into the SDK and from there to these sdk C functions (SDKX site). Host libopus instead
 * of the HLE hwopus service (opus_abi.cpp; without NNSHIM_WITH_OPUS the adapters leave the call
 * to the SDK). */
SHIM(SDKX, NATIVE, "_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEii",                        nnshim_opus_GetHardwareOpusDecoderWorkBufferSize)
SHIM(SDKX, NATIVE, "_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", nnshim_opus_InitializeHardwareOpusDecoder)
SHIM(SDKX, NATIVE, "_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", nnshim_opus_DecodeOpusInterleavedWithHardware)
SHIM(SDKX, NATIVE, "_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE",    nnshim_opus_FinalizeHardwareOpusDecoder)
SHIM(SDKX, NATIVE, "_ZN2nn5codec19HardwareOpusDecoder8FinalizeEv",                                 nnshim_opus_HardwareOpusDecoder_Finalize)
SHIM(PLT, PASS,   "_ZN2nn5codec19HardwareOpusDecoderC1Ei",                                         none)

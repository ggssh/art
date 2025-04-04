/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "parsed_options.h"

#include <memory>
#include <sstream>

#include <android-base/logging.h>
#include <android-base/strings.h>

#include "base/file_utils.h"
#include "base/flags.h"
#include "base/indenter.h"
#include "base/macros.h"
#include "base/utils.h"
#include "debugger.h"
#include "gc/heap.h"
#include "jni_id_type.h"
#include "monitor.h"
#include "runtime.h"
#include "ti/agent.h"
#include "trace.h"

#include "cmdline_parser.h"
#include "runtime_options.h"

namespace art HIDDEN {

using MemoryKiB = Memory<1024>;

ParsedOptions::ParsedOptions()
  : hook_is_sensitive_thread_(nullptr),
    hook_vfprintf_(vfprintf),
    hook_exit_(exit),
    hook_abort_(nullptr) {                          // We don't call abort(3) by default; see
                                                    // Runtime::Abort
}

bool ParsedOptions::Parse(const RuntimeOptions& options,
                          bool ignore_unrecognized,
                          RuntimeArgumentMap* runtime_options) {
  CHECK(runtime_options != nullptr);

  ParsedOptions parser;
  return parser.DoParse(options, ignore_unrecognized, runtime_options);
}

using RuntimeParser = CmdlineParser<RuntimeArgumentMap, RuntimeArgumentMap::Key>;
using HiddenapiPolicyValueMap =
    std::initializer_list<std::pair<const char*, hiddenapi::EnforcementPolicy>>;

// Yes, the stack frame is huge. But we get called super early on (and just once)
// to pass the command line arguments, so we'll probably be ok.
// Ideas to avoid suppressing this diagnostic are welcome!
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-larger-than="

std::unique_ptr<RuntimeParser> ParsedOptions::MakeParser(bool ignore_unrecognized) {
  using M = RuntimeArgumentMap;

  std::unique_ptr<RuntimeParser::Builder> parser_builder =
      std::make_unique<RuntimeParser::Builder>();

  HiddenapiPolicyValueMap hiddenapi_policy_valuemap =
      {{"disabled",  hiddenapi::EnforcementPolicy::kDisabled},
       {"just-warn", hiddenapi::EnforcementPolicy::kJustWarn},
       {"enabled",   hiddenapi::EnforcementPolicy::kEnabled}};
  DCHECK_EQ(hiddenapi_policy_valuemap.size(),
            static_cast<size_t>(hiddenapi::EnforcementPolicy::kMax) + 1);

  // clang-format off
  parser_builder->
       SetCategory("standard")
      .Define({"-classpath _", "-cp _"})
          .WithHelp("The classpath, separated by ':'")
          .WithType<std::string>()
          .IntoKey(M::ClassPath)
      .Define("-D_")
          .WithType<std::vector<std::string>>().AppendValues()
          .IntoKey(M::PropertiesList)
      .Define("-verbose:_")
          .WithHelp("Switches for advanced logging. Multiple categories can be enabled separated by ','. Eg: -verbose:class,deopt")
          .WithType<LogVerbosity>()
          .IntoKey(M::Verbose)
      .Define({"-help", "-h"})
          .WithHelp("Print this help text.")
          .IntoKey(M::Help)
      .Define("-showversion")
          .IntoKey(M::ShowVersion)
      // TODO Re-enable -agentlib: once I have a good way to transform the values.
      // .Define("-agentlib:_")
      //     .WithType<std::vector<ti::Agent>>().AppendValues()
      //     .IntoKey(M::AgentLib)
      .Define("-agentpath:_")
          .WithHelp("Load native agents.")
          .WithType<std::list<ti::AgentSpec>>().AppendValues()
          .IntoKey(M::AgentPath)
      .SetCategory("extended")
      .Define("-Xbootclasspath:_")
          .WithType<ParseStringList<':'>>()  // std::vector<std::string>, split by :
          .IntoKey(M::BootClassPath)
      .Define("-Xbootclasspathfds:_")
          .WithType<ParseIntList<':'>>()
          .IntoKey(M::BootClassPathFds)
      .Define("-Xbootclasspathimagefds:_")
          .WithType<ParseIntList<':'>>()
          .IntoKey(M::BootClassPathImageFds)
      .Define("-Xbootclasspathvdexfds:_")
          .WithType<ParseIntList<':'>>()
          .IntoKey(M::BootClassPathVdexFds)
      .Define("-Xbootclasspathoatfds:_")
          .WithType<ParseIntList<':'>>()
          .IntoKey(M::BootClassPathOatFds)
      .Define("-Xcheck:jni")
          .IntoKey(M::CheckJni)
      .Define("-Xms_")
          .WithType<MemoryKiB>()
          .IntoKey(M::MemoryInitialSize)
      .Define("-Xmx_")
          .WithType<MemoryKiB>()
          .IntoKey(M::MemoryMaximumSize)
      .Define("-Xss_")
          .WithType<Memory<1>>()
          .IntoKey(M::StackSize)
      .Define("-Xint")
          .WithValue(true)
          .IntoKey(M::Interpret)
      .SetCategory("Dalvik")
      .Define("-Xzygote")
          .WithHelp("Start as zygote")
          .IntoKey(M::Zygote)
      .Define("-Xjnitrace:_")
          .WithType<std::string>()
          .IntoKey(M::JniTrace)
      .Define("-Xgc:_")
          .WithType<XGcOption>()
          .IntoKey(M::GcOption)
      .Define("-XX:HeapGrowthLimit=_")
          .WithType<MemoryKiB>()
          .IntoKey(M::HeapGrowthLimit)
      .Define("-XX:HeapMinFree=_")
          .WithType<MemoryKiB>()
          .IntoKey(M::HeapMinFree)
      .Define("-XX:HeapMaxFree=_")
          .WithType<MemoryKiB>()
          .IntoKey(M::HeapMaxFree)
      .Define("-XX:NonMovingSpaceCapacity=_")
          .WithType<MemoryKiB>()
          .IntoKey(M::NonMovingSpaceCapacity)
      .Define("-XX:HeapTargetUtilization=_")
          .WithType<double>().WithRange(0.1, 0.9)
          .IntoKey(M::HeapTargetUtilization)
      .Define("-XX:ForegroundHeapGrowthMultiplier=_")
          .WithType<double>().WithRange(0.1, 5.0)
          .IntoKey(M::ForegroundHeapGrowthMultiplier)
      .Define("-XX:LowMemoryMode")
          .IntoKey(M::LowMemoryMode)
      .Define("-Xjitthreshold:_")
          .WithType<unsigned int>()
          .IntoKey(M::JITOptimizeThreshold)
      .SetCategory("ART")
      .Define("-Ximage:_")
          .WithType<ParseStringList<':'>>()
          .IntoKey(M::Image)
      .Define("-Xforcejitzygote")
          .IntoKey(M::ForceJitZygote)
      .Define("-Xallowinmemorycompilation")
          .WithHelp("Allows compiling the boot classpath in memory when the given boot image is"
              "unusable. This option is set by default for Zygote.")
          .IntoKey(M::AllowInMemoryCompilation)
      .Define("-Xprimaryzygote")
          .IntoKey(M::PrimaryZygote)
      .Define("-Xbootclasspath-locations:_")
          .WithType<ParseStringList<':'>>()  // std::vector<std::string>, split by :
          .IntoKey(M::BootClassPathLocations)
      .Define("-Xjniopts:forcecopy")
          .IntoKey(M::JniOptsForceCopy)
      .Define("-XjdwpProvider:_")
          .WithType<JdwpProvider>()
          .IntoKey(M::JdwpProvider)
      .Define("-XjdwpOptions:_")
          .WithMetavar("OPTION[,OPTION...]")
          .WithHelp("JDWP options. Eg suspend=n,server=y.")
          .WithType<std::string>()
          .IntoKey(M::JdwpOptions)
      .Define("-XX:StopForNativeAllocs=_")
          .WithType<MemoryKiB>()
          .IntoKey(M::StopForNativeAllocs)
      .Define("-XX:ParallelGCThreads=_")
          .WithType<unsigned int>()
          .IntoKey(M::ParallelGCThreads)
      .Define("-XX:ConcGCThreads=_")
          .WithType<unsigned int>()
          .IntoKey(M::ConcGCThreads)
      .Define("-XX:FinalizerTimeoutMs=_")
          .WithType<unsigned int>()
          .IntoKey(M::FinalizerTimeoutMs)
      .Define("-XX:MaxSpinsBeforeThinLockInflation=_")
          .WithType<unsigned int>()
          .IntoKey(M::MaxSpinsBeforeThinLockInflation)
      .Define("-XX:LongPauseLogThreshold=_")  // in ms
          .WithType<MillisecondsToNanoseconds>()  // store as ns
          .IntoKey(M::LongPauseLogThreshold)
      .Define("-XX:LongGCLogThreshold=_")  // in ms
          .WithType<MillisecondsToNanoseconds>()  // store as ns
          .IntoKey(M::LongGCLogThreshold)
      .Define("-XX:DumpGCPerformanceOnShutdown")
          .IntoKey(M::DumpGCPerformanceOnShutdown)
      .Define("-XX:DumpRegionInfoBeforeGC")
          .IntoKey(M::DumpRegionInfoBeforeGC)
      .Define("-XX:DumpRegionInfoAfterGC")
          .IntoKey(M::DumpRegionInfoAfterGC)
      .Define("-XX:DumpJITInfoOnShutdown")
          .IntoKey(M::DumpJITInfoOnShutdown)
      .Define("-XX:IgnoreMaxFootprint")
          .IntoKey(M::IgnoreMaxFootprint)
      .Define("-XX:AlwaysLogExplicitGcs:_")
          .WithHelp("Allows one to control the logging of explicit GCs. Defaults to 'true'")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::AlwaysLogExplicitGcs)
      .Define("-XX:UseTLAB")
          .WithValue(true)
          .IntoKey(M::UseTLAB)
      .Define({"-XX:EnableHSpaceCompactForOOM", "-XX:DisableHSpaceCompactForOOM"})
          .WithValues({true, false})
          .IntoKey(M::EnableHSpaceCompactForOOM)
      .Define("-XX:DumpNativeStackOnSigQuit:_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::DumpNativeStackOnSigQuit)
      .Define("-XX:MadviseRandomAccess:_")
          .WithHelp("Deprecated option")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::MadviseRandomAccess)
      .Define("-XMadviseWillNeedVdexFileSize:_")
          .WithType<unsigned int>()
          .IntoKey(M::MadviseWillNeedVdexFileSize)
      .Define("-XMadviseWillNeedOdexFileSize:_")
          .WithType<unsigned int>()
          .IntoKey(M::MadviseWillNeedOdexFileSize)
      .Define("-XMadviseWillNeedArtFileSize:_")
          .WithType<unsigned int>()
          .IntoKey(M::MadviseWillNeedArtFileSize)
      .Define("-Xusejit:_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::UseJitCompilation)
      .Define("-Xuseprofiledjit:_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::UseProfiledJitCompilation)
      .Define("-Xjitinitialsize:_")
          .WithType<MemoryKiB>()
          .IntoKey(M::JITCodeCacheInitialCapacity)
      .Define("-Xjitmaxsize:_")
          .WithType<MemoryKiB>()
          .IntoKey(M::JITCodeCacheMaxCapacity)
      .Define("-Xjitwarmupthreshold:_")
          .WithType<unsigned int>()
          .IntoKey(M::JITWarmupThreshold)
      .Define("-Xjitprithreadweight:_")
          .WithType<unsigned int>()
          .IntoKey(M::JITPriorityThreadWeight)
      .Define("-Xjittransitionweight:_")
          .WithType<unsigned int>()
          .IntoKey(M::JITInvokeTransitionWeight)
      .Define("-Xjitpthreadpriority:_")
          .WithType<int>()
          .IntoKey(M::JITPoolThreadPthreadPriority)
      .Define("-Xjitzygotepthreadpriority:_")
          .WithType<int>()
          .IntoKey(M::JITZygotePoolThreadPthreadPriority)
      .Define("-Xjitsaveprofilinginfo")
          .WithType<ProfileSaverOptions>()
          .AppendValues()
          .IntoKey(M::ProfileSaverOpts)
      // .Define("-Xps-_")  // profile saver options -Xps-<key>:<value>
      //     .WithType<ProfileSaverOptions>()
      //     .AppendValues()
      //     .IntoKey(M::ProfileSaverOpts)  // NOTE: Appends into same key as -Xjitsaveprofilinginfo
      // profile saver options -Xps-<key>:<value> but are split-out for better help messages.
      // The order of these is important. We want the wildcard one to be the
      // only one actually matched so it needs to be first.
      // TODO This should be redone.
      .Define({"-Xps-_",
               "-Xps-min-save-period-ms:_",
               "-Xps-min-first-save-ms:_",
               "-Xps-save-resolved-classes-delayed-ms:_",
               "-Xps-hot-startup-method-samples:_",
               "-Xps-min-methods-to-save:_",
               "-Xps-min-classes-to-save:_",
               "-Xps-min-notification-before-wake:_",
               "-Xps-max-notification-before-wake:_",
               "-Xps-inline-cache-threshold:_",
               "-Xps-profile-path:_"})
          .WithHelp("profile-saver options -Xps-<key>:<value>")
          .WithType<ProfileSaverOptions>()
          .AppendValues()
          .IntoKey(M::ProfileSaverOpts)  // NOTE: Appends into same key as -Xjitsaveprofilinginfo
      .Define("-XX:HspaceCompactForOOMMinIntervalMs=_")  // in ms
          .WithType<MillisecondsToNanoseconds>()  // store as ns
          .IntoKey(M::HSpaceCompactForOOMMinIntervalsMs)
      .Define({"-Xrelocate", "-Xnorelocate"})
          .WithValues({true, false})
          .IntoKey(M::Relocate)
      .Define({"-Ximage-dex2oat", "-Xnoimage-dex2oat"})
          .WithValues({true, false})
          .IntoKey(M::ImageDex2Oat)
      .Define("-XX:LargeObjectSpace=_")
          .WithType<gc::space::LargeObjectSpaceType>()
          .WithValueMap({{"disabled", gc::space::LargeObjectSpaceType::kDisabled},
                         {"freelist", gc::space::LargeObjectSpaceType::kFreeList},
                         {"map",      gc::space::LargeObjectSpaceType::kMap}})
          .IntoKey(M::LargeObjectSpace)
      .Define("-XX:LargeObjectThreshold=_")
          .WithType<Memory<1>>()
          .IntoKey(M::LargeObjectThreshold)
      .Define("-XX:BackgroundGC=_")
          .WithType<BackgroundGcOption>()
          .IntoKey(M::BackgroundGc)
      .Define("-XX:+DisableExplicitGC")
          .IntoKey(M::DisableExplicitGC)
      .Define("-XX:+DisableEagerlyReleaseExplicitGC")
          .IntoKey(M::DisableEagerlyReleaseExplicitGC)
      .Define("-Xlockprofthreshold:_")
          .WithType<unsigned int>()
          .IntoKey(M::LockProfThreshold)
      .Define("-Xstackdumplockprofthreshold:_")
          .WithType<unsigned int>()
          .IntoKey(M::StackDumpLockProfThreshold)
      .Define("-Xmethod-trace")
          .IntoKey(M::MethodTrace)
      .Define("-Xmethod-trace-file:_")
          .WithType<std::string>()
          .IntoKey(M::MethodTraceFile)
      .Define("-Xmethod-trace-file-size:_")
          .WithType<unsigned int>()
          .IntoKey(M::MethodTraceFileSize)
      .Define("-Xmethod-trace-stream")
          .IntoKey(M::MethodTraceStreaming)
      .Define("-Xmethod-trace-clock:_")
          .WithType<TraceClockSource>()
          .WithValueMap({{"threadcpuclock", TraceClockSource::kThreadCpu},
                         {"wallclock",      TraceClockSource::kWall},
                         {"dualclock",      TraceClockSource::kDual}})
          .IntoKey(M::MethodTraceClock)
      .Define("-Xcompiler:_")
          .WithType<std::string>()
          .IntoKey(M::Compiler)
      .Define("-Xcompiler-option _")
          .WithType<std::vector<std::string>>()
          .AppendValues()
          .IntoKey(M::CompilerOptions)
      .Define("-Ximage-compiler-option _")
          .WithType<std::vector<std::string>>()
          .AppendValues()
          .IntoKey(M::ImageCompilerOptions)
      .Define("-Xverify:_")
          .WithType<verifier::VerifyMode>()
          .WithValueMap({{"none",     verifier::VerifyMode::kNone},
                         {"remote",   verifier::VerifyMode::kEnable},
                         {"all",      verifier::VerifyMode::kEnable},
                         {"softfail", verifier::VerifyMode::kSoftFail}})
          .IntoKey(M::Verify)
      .Define("-XX:NativeBridge=_")
          .WithType<std::string>()
          .IntoKey(M::NativeBridge)
      .Define("-Xzygote-max-boot-retry=_")
          .WithType<unsigned int>()
          .IntoKey(M::ZygoteMaxFailedBoots)
      .Define("-Xno-sig-chain")
          .IntoKey(M::NoSigChain)
      .Define("--cpu-abilist=_")
          .WithType<std::string>()
          .IntoKey(M::CpuAbiList)
      .Define("-Xfingerprint:_")
          .WithType<std::string>()
          .IntoKey(M::Fingerprint)
      .Define("-Xexperimental:_")
          .WithType<ExperimentalFlags>()
          .AppendValues()
          .IntoKey(M::Experimental)
      .Define("-Xforce-nb-testing")
          .IntoKey(M::ForceNativeBridge)
      .Define("-Xplugin:_")
          .WithHelp("Load and initialize the specified art-plugin.")
          .WithType<std::vector<Plugin>>().AppendValues()
          .IntoKey(M::Plugins)
      .Define("-XX:ThreadSuspendTimeout=_")  // in ms
          .WithType<MillisecondsToNanoseconds>()  // store as ns
          .IntoKey(M::ThreadSuspendTimeout)
      .Define("-XX:MonitorTimeoutEnable=_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::MonitorTimeoutEnable)
      .Define("-XX:MonitorTimeout=_")  // in ms
          .WithType<int>()
          .IntoKey(M::MonitorTimeout)
      .Define("-XX:GlobalRefAllocStackTraceLimit=_")  // Number of free slots to enable tracing.
          .WithType<unsigned int>()
          .IntoKey(M::GlobalRefAllocStackTraceLimit)
      .Define("-XX:SlowDebug=_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::SlowDebug)
      .Define("-Xtarget-sdk-version:_")
          .WithType<unsigned int>()
          .IntoKey(M::TargetSdkVersion)
      .Define("-Xhidden-api-policy:_")
          .WithType<hiddenapi::EnforcementPolicy>()
          .WithValueMap(hiddenapi_policy_valuemap)
          .IntoKey(M::HiddenApiPolicy)
      .Define("-Xcore-platform-api-policy:_")
          .WithType<hiddenapi::EnforcementPolicy>()
          .WithValueMap(hiddenapi_policy_valuemap)
          .IntoKey(M::CorePlatformApiPolicy)
      .Define("-Xuse-stderr-logger")
          .IntoKey(M::UseStderrLogger)
      .Define("-Xonly-use-system-oat-files")
          .IntoKey(M::OnlyUseTrustedOatFiles)
      .Define("-Xdeny-art-apex-data-files")
          .IntoKey(M::DenyArtApexDataFiles)
      .Define("-Xverifier-logging-threshold=_")
          .WithType<unsigned int>()
          .IntoKey(M::VerifierLoggingThreshold)
      .Define("-XX:FastClassNotFoundException=_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::FastClassNotFoundException)
      .Define("-Xopaque-jni-ids:_")
          .WithHelp("Control the representation of jmethodID and jfieldID values")
          .WithType<JniIdType>()
          .WithValueMap({{"true", JniIdType::kIndices},
                         {"false", JniIdType::kPointer},
                         {"swapable", JniIdType::kSwapablePointer},
                         {"pointer", JniIdType::kPointer},
                         {"indices", JniIdType::kIndices},
                         {"default", JniIdType::kDefault}})
          .IntoKey(M::OpaqueJniIds)
      .Define("-Xauto-promote-opaque-jni-ids:_")
          .WithType<bool>()
          .WithValueMap({{"true", true}, {"false", false}})
          .IntoKey(M::AutoPromoteOpaqueJniIds)
      .Define("-XX:VerifierMissingKThrowFatal=_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::VerifierMissingKThrowFatal)
      .Define("-XX:ForceJavaZygoteForkLoop=_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::ForceJavaZygoteForkLoop)
      .Define("-XX:PerfettoHprof=_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::PerfettoHprof)
      .Define("-XX:PerfettoJavaHeapStackProf=_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::PerfettoJavaHeapStackProf);
  // clang-format on

  FlagBase::AddFlagsToCmdlineParser(parser_builder.get());

  parser_builder
      ->Ignore({"-ea",
                "-da",
                "-enableassertions",
                "-disableassertions",
                "--runtime-arg",
                "-esa",
                "-dsa",
                "-enablesystemassertions",
                "-disablesystemassertions",
                "-Xrs",
                "-Xint:_",
                "-Xdexopt:_",
                "-Xnoquithandler",
                "-Xjnigreflimit:_",
                "-Xgenregmap",
                "-Xnogenregmap",
                "-Xverifyopt:_",
                "-Xcheckdexsum",
                "-Xincludeselectedop",
                "-Xjitop:_",
                "-Xincludeselectedmethod",
                "-Xjitblocking",
                "-Xjitmethod:_",
                "-Xjitclass:_",
                "-Xjitoffset:_",
                "-Xjitosrthreshold:_",
                "-Xjitconfig:_",
                "-Xjitcheckcg",
                "-Xjitverbose",
                "-Xjitprofile",
                "-Xjitdisableopt",
                "-Xjitsuspendpoll",
                "-XX:mainThreadStackSize=_",
                "-Xprofile:_"})
      .IgnoreUnrecognized(ignore_unrecognized)
      .OrderCategories({"standard", "extended", "Dalvik", "ART"});

  // TODO: Move Usage information into this DSL.

  return std::make_unique<RuntimeParser>(parser_builder->Build());
}

#pragma GCC diagnostic pop

// Remove all the special options that have something in the void* part of the option.
// If runtime_options is not null, put the options in there.
// As a side-effect, populate the hooks from options.
bool ParsedOptions::ProcessSpecialOptions(const RuntimeOptions& options,
                                          RuntimeArgumentMap* runtime_options,
                                          std::vector<std::string>* out_options) {
  using M = RuntimeArgumentMap;

  // TODO: Move the below loop into JNI
  // Handle special options that set up hooks
  for (size_t i = 0; i < options.size(); ++i) {
    const std::string option(options[i].first);
      // TODO: support -Djava.class.path
    if (option == "bootclasspath") {
      auto boot_class_path = static_cast<std::vector<std::unique_ptr<const DexFile>>*>(
          const_cast<void*>(options[i].second));

      if (runtime_options != nullptr) {
        runtime_options->Set(M::BootClassPathDexList, boot_class_path);
      }
    } else if (option == "compilercallbacks") {
      CompilerCallbacks* compiler_callbacks =
          reinterpret_cast<CompilerCallbacks*>(const_cast<void*>(options[i].second));
      if (runtime_options != nullptr) {
        runtime_options->Set(M::CompilerCallbacksPtr, compiler_callbacks);
      }
    } else if (option == "imageinstructionset") {
      const char* isa_str = reinterpret_cast<const char*>(options[i].second);
      auto&& image_isa = GetInstructionSetFromString(isa_str);
      if (image_isa == InstructionSet::kNone) {
        Usage("%s is not a valid instruction set.", isa_str);
        return false;
      }
      if (runtime_options != nullptr) {
        runtime_options->Set(M::ImageInstructionSet, image_isa);
      }
    } else if (option == "sensitiveThread") {
      const void* hook = options[i].second;
      bool (*hook_is_sensitive_thread)() = reinterpret_cast<bool (*)()>(const_cast<void*>(hook));

      if (runtime_options != nullptr) {
        runtime_options->Set(M::HookIsSensitiveThread, hook_is_sensitive_thread);
      }
    } else if (option == "vfprintf") {
      const void* hook = options[i].second;
      if (hook == nullptr) {
        Usage("vfprintf argument was nullptr");
        return false;
      }
      int (*hook_vfprintf)(FILE *, const char*, va_list) =
          reinterpret_cast<int (*)(FILE *, const char*, va_list)>(const_cast<void*>(hook));

      if (runtime_options != nullptr) {
        runtime_options->Set(M::HookVfprintf, hook_vfprintf);
      }
      hook_vfprintf_ = hook_vfprintf;
    } else if (option == "exit") {
      const void* hook = options[i].second;
      if (hook == nullptr) {
        Usage("exit argument was nullptr");
        return false;
      }
      void(*hook_exit)(jint) = reinterpret_cast<void(*)(jint)>(const_cast<void*>(hook));
      if (runtime_options != nullptr) {
        runtime_options->Set(M::HookExit, hook_exit);
      }
      hook_exit_ = hook_exit;
    } else if (option == "abort") {
      const void* hook = options[i].second;
      if (hook == nullptr) {
        Usage("abort was nullptr\n");
        return false;
      }
      void(*hook_abort)() = reinterpret_cast<void(*)()>(const_cast<void*>(hook));
      if (runtime_options != nullptr) {
        runtime_options->Set(M::HookAbort, hook_abort);
      }
      hook_abort_ = hook_abort;
    } else {
      // It is a regular option, that doesn't have a known 'second' value.
      // Push it on to the regular options which will be parsed by our parser.
      if (out_options != nullptr) {
        out_options->push_back(option);
      }
    }
  }

  return true;
}

// Intended for local changes only.
static void MaybeOverrideVerbosity() {
  //  gLogVerbosity.class_linker = true;  // TODO: don't check this in!
  //  gLogVerbosity.collector = true;  // TODO: don't check this in!
  //  gLogVerbosity.compiler = true;  // TODO: don't check this in!
  //  gLogVerbosity.deopt = true;  // TODO: don't check this in!
  //  gLogVerbosity.gc = true;  // TODO: don't check this in!
  //  gLogVerbosity.heap = true;  // TODO: don't check this in!
  //  gLogVerbosity.image = true;  // TODO: don't check this in!
  //  gLogVerbosity.interpreter = true;  // TODO: don't check this in!
  //  gLogVerbosity.jdwp = true;  // TODO: don't check this in!
  //  gLogVerbosity.jit = true;  // TODO: don't check this in!
  //  gLogVerbosity.jni = true;  // TODO: don't check this in!
  //  gLogVerbosity.monitor = true;  // TODO: don't check this in!
  //  gLogVerbosity.oat = true;  // TODO: don't check this in!
  //  gLogVerbosity.profiler = true;  // TODO: don't check this in!
  //  gLogVerbosity.signals = true;  // TODO: don't check this in!
  //  gLogVerbosity.simulator = true; // TODO: don't check this in!
  //  gLogVerbosity.startup = true;  // TODO: don't check this in!
  //  gLogVerbosity.third_party_jni = true;  // TODO: don't check this in!
  //  gLogVerbosity.threads = true;  // TODO: don't check this in!
  //  gLogVerbosity.verifier = true;  // TODO: don't check this in!
}

bool ParsedOptions::DoParse(const RuntimeOptions& options,
                            bool ignore_unrecognized,
                            RuntimeArgumentMap* runtime_options) {
  for (size_t i = 0; i < options.size(); ++i) {
    if (true && options[0].first == "-Xzygote") {
      LOG(INFO) << "option[" << i << "]=" << options[i].first;
    }
  }

  auto parser = MakeParser(ignore_unrecognized);

  // Convert to a simple string list (without the magic pointer options)
  std::vector<std::string> argv_list;
  if (!ProcessSpecialOptions(options, nullptr, &argv_list)) {
    return false;
  }

  CmdlineResult parse_result = parser->Parse(argv_list);

  // Handle parse errors by displaying the usage and potentially exiting.
  if (parse_result.IsError()) {
    if (parse_result.GetStatus() == CmdlineResult::kUsage) {
      UsageMessage(stdout, "%s\n", parse_result.GetMessage().c_str());
      Exit(0);
    } else if (parse_result.GetStatus() == CmdlineResult::kUnknown && !ignore_unrecognized) {
      Usage("%s\n", parse_result.GetMessage().c_str());
      return false;
    } else {
      Usage("%s\n", parse_result.GetMessage().c_str());
      Exit(0);
    }

    UNREACHABLE();
  }

  using M = RuntimeArgumentMap;
  RuntimeArgumentMap args = parser->ReleaseArgumentsMap();
  bool use_default_bootclasspath = true;

  // -help, -showversion, etc.
  if (args.Exists(M::Help)) {
    Usage(nullptr);
    return false;
  } else if (args.Exists(M::ShowVersion)) {
    UsageMessage(stdout,
                 "ART version %s %s\n",
                 Runtime::GetVersion(),
                 GetInstructionSetString(kRuntimeISA));
    Exit(0);
  } else if (args.Exists(M::BootClassPath)) {
    LOG(INFO) << "setting boot class path to " << args.Get(M::BootClassPath)->Join();
    use_default_bootclasspath = false;
  }

  if (args.GetOrDefault(M::Interpret)) {
    if (args.Exists(M::UseJitCompilation) && *args.Get(M::UseJitCompilation)) {
      Usage("-Xusejit:true and -Xint cannot be specified together\n");
      Exit(0);
    }
    args.Set(M::UseJitCompilation, false);
  }

  // Set a default boot class path if we didn't get an explicit one via command line.
  const char* env_bcp = getenv("BOOTCLASSPATH");
  if (env_bcp != nullptr) {
    args.SetIfMissing(M::BootClassPath, ParseStringList<':'>::Split(env_bcp));
  }

  // Set a default class path if we didn't get an explicit one via command line.
  if (getenv("CLASSPATH") != nullptr) {
    args.SetIfMissing(M::ClassPath, std::string(getenv("CLASSPATH")));
  }

  // Default to number of processors minus one since the main GC thread also does work.
  args.SetIfMissing(M::ParallelGCThreads, gc::Heap::kDefaultEnableParallelGC ?
      static_cast<unsigned int>(sysconf(_SC_NPROCESSORS_CONF) - 1u) : 0u);

  // -verbose:
  {
    LogVerbosity *log_verbosity = args.Get(M::Verbose);
    if (log_verbosity != nullptr) {
      gLogVerbosity = *log_verbosity;
    }
  }

  MaybeOverrideVerbosity();

  SetRuntimeDebugFlagsEnabled(args.GetOrDefault(M::SlowDebug));

  if (!ProcessSpecialOptions(options, &args, nullptr)) {
      return false;
  }

  {
    // If not set, background collector type defaults to homogeneous compaction.
    // If not low memory mode, semispace otherwise.

    gc::CollectorType background_collector_type_ = args.GetOrDefault(M::BackgroundGc);
    bool low_memory_mode_ = args.Exists(M::LowMemoryMode);

    if (background_collector_type_ == gc::kCollectorTypeNone) {
      background_collector_type_ = low_memory_mode_ ?
          gc::kCollectorTypeSS : gc::kCollectorTypeHomogeneousSpaceCompact;
    }

    args.Set(M::BackgroundGc, BackgroundGcOption { background_collector_type_ });
  }

  const ParseStringList<':'>* boot_class_path_locations = args.Get(M::BootClassPathLocations);
  if (boot_class_path_locations != nullptr && boot_class_path_locations->Size() != 0u) {
    const ParseStringList<':'>* boot_class_path = args.Get(M::BootClassPath);
    if (boot_class_path == nullptr ||
        boot_class_path_locations->Size() != boot_class_path->Size()) {
      Usage("The number of boot class path files does not match"
          " the number of boot class path locations given\n"
          "  boot class path files     (%zu): %s\n"
          "  boot class path locations (%zu): %s\n",
          (boot_class_path != nullptr) ? boot_class_path->Size() : 0u,
          (boot_class_path != nullptr) ? boot_class_path->Join().c_str() : "<nil>",
          boot_class_path_locations->Size(),
          boot_class_path_locations->Join().c_str());
      return false;
    }
  }

  if (args.Exists(M::ForceJitZygote)) {
    if (args.Exists(M::Image)) {
      Usage("-Ximage and -Xforcejitzygote cannot be specified together\n");
      Exit(0);
    }
    // If `boot.art` exists in the ART APEX, it will be used. Otherwise, Everything will be JITed.
    args.Set(M::Image, ParseStringList<':'>::Split(GetJitZygoteBootImageLocation()));
  }

  if (args.Exists(M::Zygote)) {
    args.Set(M::AllowInMemoryCompilation, Unit());
  }

  if (!args.Exists(M::CompilerCallbacksPtr) && !args.Exists(M::Image) &&
      use_default_bootclasspath) {
    const bool deny_art_apex_data_files = args.Exists(M::DenyArtApexDataFiles);
    std::string image_locations =
        GetDefaultBootImageLocation(GetAndroidRoot(), deny_art_apex_data_files);
    args.Set(M::Image, ParseStringList<':'>::Split(image_locations));
  }

  // 0 means no growth limit, and growth limit should be always <= heap size
  if (args.GetOrDefault(M::HeapGrowthLimit) <= 0u ||
      args.GetOrDefault(M::HeapGrowthLimit) > args.GetOrDefault(M::MemoryMaximumSize)) {
    args.Set(M::HeapGrowthLimit, args.GetOrDefault(M::MemoryMaximumSize));
  }

  // Increase log thresholds for GC stress mode to avoid excessive log spam.
  if (args.GetOrDefault(M::GcOption).gcstress_) {
    args.SetIfMissing(M::AlwaysLogExplicitGcs, false);
    args.SetIfMissing(M::LongPauseLogThreshold, gc::Heap::kDefaultLongPauseLogThresholdGcStress);
    args.SetIfMissing(M::LongGCLogThreshold, gc::Heap::kDefaultLongGCLogThresholdGcStress);
  }

  *runtime_options = std::move(args);
  return true;
}

void ParsedOptions::Exit(int status) {
  hook_exit_(status);
}

void ParsedOptions::Abort() {
  hook_abort_();
}

void ParsedOptions::UsageMessageV(FILE* stream, const char* fmt, va_list ap) {
  hook_vfprintf_(stream, fmt, ap);
}

void ParsedOptions::UsageMessage(FILE* stream, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageMessageV(stream, fmt, ap);
  va_end(ap);
}

void ParsedOptions::Usage(const char* fmt, ...) {
  bool error = (fmt != nullptr);
  FILE* stream = error ? stderr : stdout;

  if (fmt != nullptr) {
    va_list ap;
    va_start(ap, fmt);
    UsageMessageV(stream, fmt, ap);
    va_end(ap);
  }

  const char* program = "dalvikvm";
  UsageMessage(stream, "%s: [options] class [argument ...]\n", program);
  UsageMessage(stream, "\n");

  std::stringstream oss;
  VariableIndentationOutputStream vios(&oss);
  auto parser = MakeParser(false);
  parser->DumpHelp(vios);
  UsageMessage(stream, oss.str().c_str());
  Exit((error) ? 1 : 0);
}

}  // namespace art

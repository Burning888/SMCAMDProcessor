#include "AMDRyzenCPUPowerManagement.hpp"
#include <string.h>

OSDefineMetaClassAndStructors(AMDRyzenCPUPowerManagement, IOService);

#define TCTL_OFFSET_TABLE_LEN 6
static constexpr const struct tctl_offset tctl_offset_table[] = {
    { 0x17, "AMD Ryzen 5 1600X", 20 },
    { 0x17, "AMD Ryzen 7 1700X", 20 },
    { 0x17, "AMD Ryzen 7 1800X", 20 },
    { 0x17, "AMD Ryzen 7 2700X", 10 },
    { 0x17, "AMD Ryzen Threadripper 19", 27 }, /* 19{00,20,50}X */
    { 0x17, "AMD Ryzen Threadripper 29", 27 }, /* 29{20,50,70,90}[W]X */
};

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;


extern "C"{
void pmRyzen_wrmsr_safe(void *handle, uint32_t addr, uint64_t value){
    static_cast<AMDRyzenCPUPowerManagement*>(handle)->write_msr(addr, value);
}

uint64_t pmRyzen_rdmsr_safe(void *handle, uint32_t addr){
    uint64_t v = 0;
    static_cast<AMDRyzenCPUPowerManagement*>(handle)->read_msr(addr, &v);
    return v;
}

}


bool AMDRyzenCPUPowerManagement::init(OSDictionary *dictionary){
//    strcpy((char*)kMODULE_VERSION, xStringify(MODULE_VERSION), (uint32_t)strlen(xStringify(MODULE_VERSION)));
    IOLog("AMDCPUSupport v%s, init\n", xStringify(MODULE_VERSION));
    
    
    return IOService::init(dictionary);
}

void AMDRyzenCPUPowerManagement::free(){
    IOService::free();
}


bool AMDRyzenCPUPowerManagement::getPCIService(){
    
    
    OSDictionary *matching_dict = serviceMatching("IOPCIDevice");
    if(!matching_dict){
        IOLog("AMDCPUSupport::getPCIService: serviceMatching unable to generate matching dictonary.\n");
        return false;
    }
    
    //Wait for PCI services to init.
    waitForMatchingService(matching_dict);
    
    OSIterator *service_iter = getMatchingServices(matching_dict);
    IOPCIDevice *service = 0;
    
    if(!service_iter){
        IOLog("AMDCPUSupport::getPCIService: unable to find a matching IOPCIDevice.\n");
        return false;
    }
    
    while (true){
        OSObject *obj = service_iter->getNextObject();
        if(!obj) break;
        
        service = OSDynamicCast(IOPCIDevice, obj);
        break;
    }
    
    if(!service){
        IOLog("AMDCPUSupport::getPCIService: unable to get IOPCIDevice on host system.\n");
        return false;
    }
    
    IOLog("AMDCPUSupport::getPCIService: succeed!\n");
    fIOPCIDevice = service;
    
    return true;
}


bool AMDRyzenCPUPowerManagement::start(IOService *provider){
    
    bool success = IOService::start(provider);
    if(!success){
        IOLog("AMDCPUSupport::start failed to start. :(\n");
        return false;
    }
    
    disablePrivilegeCheck = checkKernelArgument("-amdpnopchk");
    
    uint32_t cpuid_eax = 0;
    uint32_t cpuid_ebx = 0;
    uint32_t cpuid_ecx = 0;
    uint32_t cpuid_edx = 0;
    CPUInfo::getCpuid(0, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    IOLog("AMDCPUSupport::start got CPUID: %X %X %X %X\n", cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx);
    
    if(cpuid_ebx != CPUInfo::signature_AMD_ebx
       || cpuid_ecx != CPUInfo::signature_AMD_ecx
       || cpuid_edx != CPUInfo::signature_AMD_edx){
        IOLog("AMDCPUSupport::start no AMD signature detected, failing..\n");
        
        return false;
    }
    
    CPUInfo::getCpuid(1, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpuFamily = ((cpuid_eax >> 20) & 0xff) + ((cpuid_eax >> 8) & 0xf);
    cpuModel = ((cpuid_eax >> 16) & 0xf) + ((cpuid_eax >> 4) & 0xf);
    
    //Only 17h Family are supported offically by now.
    cpuSupportedByCurrentVersion = (cpuFamily == 0x17)? 1 : 0;
    IOLog("AMDCPUSupport::start Family %02Xh, Model %02Xh\n", cpuFamily, cpuModel);
    
    CPUInfo::getCpuid(0x80000005, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpuCacheL1_perCore = (cpuid_ecx >> 24) + (cpuid_ecx >> 24);
    
    
    CPUInfo::getCpuid(0x80000006, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpuCacheL2_perCore = (cpuid_ecx >> 16);
    cpuCacheL3 = (cpuid_edx >> 18) * 512;
    IOLog("AMDCPUSupport::start L1: %u, L2: %u, L3: %u\n",
          cpuCacheL1_perCore, cpuCacheL2_perCore, cpuCacheL3);
    
    
    CPUInfo::getCpuid(0x80000007, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpbSupported = (cpuid_edx >> 9) & 0x1;

    
    CPUInfo::getCpuid(0x00000005, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    IOLog("AMDCPUSupport::start CPUID MWait: %X %X %X %X\n", cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx);
    
    uint32_t nameString[12]{};
    CPUInfo::getCpuid(0x80000002, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    nameString[0] = cpuid_eax; nameString[1] = cpuid_ebx; nameString[2] = cpuid_ecx; nameString[3] = cpuid_edx;
    CPUInfo::getCpuid(0x80000003, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    nameString[4] = cpuid_eax; nameString[5] = cpuid_ebx; nameString[6] = cpuid_ecx; nameString[7] = cpuid_edx;
    CPUInfo::getCpuid(0x80000004, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    nameString[8] = cpuid_eax; nameString[9] = cpuid_ebx; nameString[10] = cpuid_ecx; nameString[11] = cpuid_edx;
    
    IOLog("AMDCPUSupport::start Processor: %s))\n", (char*)nameString);
    
    //Check tctl temperature offset
    for(int i = 0; i < TCTL_OFFSET_TABLE_LEN; i++){
        const TempOffset *to = tctl_offset_table + i;
        if(cpuFamily == to->model && strstr((char*)nameString, to->id)){
            
            tempOffset = (float)to->offset;
            break;
        }
    }
    
    uint64_t rapl = 0;
    if(!read_msr(kMSR_RAPL_PWR_UNIT, &rapl))
        panic("AMDCPUSupport: unable to read power unit\n");
    
    pwrTimeUnit = pow((double)0.5, (double)((rapl >> 16) & 0xf));
    pwrEnegryUnit = pow((double)0.5, (double)((rapl >> 8) & 0x1f));
    IOLog("a %lld\n", (long long)(pwrTimeUnit * 10000000000));
    IOLog("b %lld\n", (long long)(pwrEnegryUnit * 10000000000));
    
    fetchOEMBaseBoardInfo();
    
//    if(!CPUInfo::getCpuTopology(cpuTopology)){
//        IOLog("AMDCPUSupport::start unable to get CPU Topology.\n");
//    }
//    IOLog("AMDCPUSupport::start got %hhu CPU(s): Physical Count: %hhu, Logical Count %hhu.\n",
//          cpuTopology.packageCount, cpuTopology.totalPhysical(), cpuTopology.totalLogical());
//
//    totalNumberOfPhysicalCores = cpuTopology.totalPhysical();
//    totalNumberOfLogicalCores = cpuTopology.totalLogical();
    
    
    void *safe_wrmsr = lookup_symbol("_wrmsr_carefully");
    if(!safe_wrmsr){
        IOLog("AMDCPUSupport::start WARN: Can't find _wrmsr_carefully, proceeding with unsafe wrmsr\n");
    } else {
        wrmsr_carefully = (int(*)(uint32_t,uint32_t,uint32_t)) safe_wrmsr;
    }
    
    void *_kunc_alert = lookup_symbol("_KUNCUserNotificationDisplayAlert");
    IOLog("kunc_alert %p\n", kunc_alert);
    if(!_kunc_alert){
        IOLog("AMDCPUSupport::start WARN: Can't find _KUNCUserNotificationDisplayAlert.\n");
    } else {
        kunc_alert =
        (kern_return_t(*)(int,unsigned,const char*,const char*,const char*,
        const char*,const char*,const char*,const char*,const char*,unsigned*))_kunc_alert;
    }
    
    cpu_to_processor = (processor_t(*)(int))lookup_symbol("_cpu_to_processor");
    processor_shutdown = (kern_return_t(*)(processor_t))lookup_symbol("_processor_exit_from_user");
    processor_startup = (kern_return_t(*)(processor_t))lookup_symbol("_processor_start_from_user");
    xnuTSCFreq = *((uint64_t*)lookup_symbol("_tscFreq"));
    
//    for(int i = 4; i< 24; i++){
//        (*processor_startup)((*cpu_to_processor)(i));
//    }
    
    IOLog("AMDCPUSupport::start trying to init PCI service...\n");
    if(!getPCIService()){
        IOLog("AMDCPUSupport::start no PCI support found, failing...\n");
        return false;
    }
    
    pmRyzen_init(this);
    
    totalNumberOfLogicalCores = pmRyzen_num_logi;
    totalNumberOfPhysicalCores = pmRyzen_num_phys;
    
    IOLog("AMDCPUSupport::start, Physical Count: %u, Logical Count %u.\n",
              totalNumberOfPhysicalCores, totalNumberOfLogicalCores);
    
    workLoop = IOWorkLoop::workLoop();
    timerEventSource = IOTimerEventSource::timerEventSource(this, [](OSObject *object, IOTimerEventSource *sender) {
        AMDRyzenCPUPowerManagement *provider = OSDynamicCast(AMDRyzenCPUPowerManagement, object);

        
        
        //Run initialization
        if(!provider->serviceInitialized){
            
            
            //Disable interrupts and sync all processor cores.
            mp_rendezvous_no_intrs([](void *obj) {
                auto provider = static_cast<AMDRyzenCPUPowerManagement*>(obj);
                
                provider->write_msr(kMSR_CSTATE_ADDR, 0xf0);
                
//                uint64_t val = 0;
//                provider->read_msr(0xC0010292, &val);
//                provider->write_msr(0xC0010292, val | ((uint64_t)1 << 32));
//
//                provider->read_msr(0xC0010296, &val);
//                provider->write_msr(0xC0010296, val | (1 << 22) | (1 << 14) | (1 << 6));

                
                
                uint64_t hwConfig;
                if(!provider->read_msr(kMSR_HWCR, &hwConfig))
                    panic("AMDCPUSupport::start: wtf?");

                hwConfig |= (1 << 30);
                provider->write_msr(kMSR_HWCR, hwConfig);


                uint32_t cpu_num = cpu_number();

                //Read PStateDef generated by EFI.
                if(pmRyzen_cpu_is_master(cpu_num))
                    provider->dumpPstate();


                if(!pmRyzen_cpu_primary_in_core(cpu_num)) return;
                uint8_t physical = pmRyzen_cpu_phys_num(cpu_num);


                //Init performance frequency counter.
                uint64_t APERF, MPERF;
                if(!provider->read_msr(kMSR_APERF, &APERF) || !provider->read_msr(kMSR_MPERF, &MPERF))
                    panic("AMDCPUSupport::start: wtf?");

                provider->lastAPERF_PerCore[physical] = APERF;
                provider->lastMPERF_PerCore[physical] = MPERF;

            }, provider);
            
            //Make all cores P0 state by default.
            provider->PStateCtl = 0;
            
            provider->serviceInitialized = true;
            provider->timerEventSource->setTimeoutMS(1);
            return;
        }
        
        
        mp_rendezvous_no_intrs([](void *obj) {
            auto provider = static_cast<AMDRyzenCPUPowerManagement*>(obj);
            uint32_t cpu_num = cpu_number();
            
            provider->updateInstructionDelta(cpu_num);
            
            // Ignore hyper-threaded cores
            if(!pmRyzen_cpu_primary_in_core(cpu_num)) return;
            uint8_t physical = pmRyzen_cpu_phys_num(cpu_num);


            provider->calculateEffectiveFrequency(physical);

        }, provider);
        
        //Read stats from package.
        provider->updatePackageTemp();
        provider->updatePackageEnergy();
        
//        IOLog("exit idle: %llu, ipi: %llu, diff %llu, false %llu\n", pmRyzen_exit_idle_c, pmRyzen_exit_idle_ipi_c, pmRyzen_exit_idle_c - pmRyzen_exit_idle_ipi_c, pmRyzen_exit_idle_false_c);
//        pmRyzen_exit_idle_c = 0; pmRyzen_exit_idle_ipi_c = 0; pmRyzen_exit_idle_false_c = 0;
//
//
//        IOLog("time: 0.%llu, running: %llu\n", uint64_t(pmRyzen_get_processor(0)->eff_load * 1000000000), pmRyzen_get_processor(4)->stat_exit_idle);
        
//        IOLog("active p %u\n", pmRyzen_hpcpus);
        
        uint32_t now = uint32_t(getCurrentTimeNs() / 1000000); //ms
        uint32_t newInt = max(now - provider->timeOfLastMissedRequest,
                              provider->estimatedRequestTimeInterval);
        
        provider->actualUpdateTimeInterval = now - provider->timeOfLastUpdate;
        provider->timeOfLastUpdate = now;
        provider->updateTimeInterval = min(1200, max(50, newInt));
        
        provider->timerEventSource->setTimeoutMS(provider->updateTimeInterval);
//        IOLog("est time: %d\n", provider->estimatedRequestTimeInterval);
//        IOLog("update time: %d\n", provider->updateTimeInterval);
//        IOLog("Core %d: %llu\n", 0, (uint64_t)(provider->PStateCur_perCore[0]));
//        for (int i = 0; i < provider->totalNumberOfPhysicalCores; i++) {
//            IOLog("Core %d: %llu\n", i, (uint64_t)(provider->PStateCur_perCore[i]));
//        }
        
        
    });
    

    registerService();
    

    
    lastUpdateTime = getCurrentTimeNs();
    pwrLastTSC = rdtsc64();
    workLoop->addEventSource(timerEventSource);
    timerEventSource->setTimeoutMS(1);
    

    
    return success;
}

void AMDRyzenCPUPowerManagement::stop(IOService *provider){
    pmRyzen_stop();
    
    IOLog("AMDCPUSupport stopped\n");
    
    timerEventSource->cancelTimeout();
    workLoop->removeEventSource(timerEventSource);
    timerEventSource->release();
    
    IOService::stop(provider);
}

void AMDRyzenCPUPowerManagement::fetchOEMBaseBoardInfo(){
    auto efiRT = EfiRuntimeServices::get();
    uint32_t att = 0;
    uint64_t sizee = BASEBOARD_STRING_MAX;
    uint64_t efistat;
    efistat = efiRT->getVariable(OC_OEM_VENDOR_VARIABLE_NAME, &EfiRuntimeServices::LiluVendorGuid,
                                 &att, &sizee, boardVender);
    
    sizee = BASEBOARD_STRING_MAX;
    efistat = efiRT->getVariable(OC_OEM_BOARD_VARIABLE_NAME, &EfiRuntimeServices::LiluVendorGuid,
                                 &att, &sizee, boardName);
    boardInfoValid = efistat == EFI_SUCCESS;
    IOLog("MB: %s %s\n", boardName, boardVender);
}

bool AMDRyzenCPUPowerManagement::read_msr(uint32_t addr, uint64_t *value){
    
    uint32_t lo, hi;
    //    IOLog("AMDCPUSupport lalala \n");
    int err = rdmsr_carefully(addr, &lo, &hi);
    //    IOLog("AMDCPUSupport rdmsr_carefully %d\n", err);
    
    if(!err) *value = lo | ((uint64_t)hi << 32);
    
    return err == 0;
}

bool AMDRyzenCPUPowerManagement::write_msr(uint32_t addr, uint64_t value){
    if(wrmsr_carefully){
        uint32_t lo = value & 0xffffffff;
        uint32_t hi = value >> 32;
        return (*wrmsr_carefully)(addr, lo, hi) == 0;
    }
    
    //Fall back with unsafe method
    wrmsr64(addr, value);
    
    //If failed, we've already panic and starting reboot. So just return true.
    return true;
}

void AMDRyzenCPUPowerManagement::registerRequest(){
    uint32_t now = (uint32_t)(getCurrentTimeNs() / 1000000);
    
    estimatedRequestTimeInterval = now - timeOfLastMissedRequest;
    timeOfLastMissedRequest = now;
}

void AMDRyzenCPUPowerManagement::updateClockSpeed(uint8_t physical){
    
    uint64_t msr_value_buf = 0;
    bool err = !read_msr(kMSR_HARDWARE_PSTATE_STATUS, &msr_value_buf);
    if(err) panic("AMDCPUSupport::updateClockSpeed: fucked up");
    
    //Convert register value to clock speed.
    uint32_t eax = (uint32_t)(msr_value_buf & 0xffffffff);
    
    // MSRC001_0293
    // CurHwPstate [24:22]
    // CurCpuVid [21:14]
    // CurCpuDfsId [13:8]
    // CurCpuFid [7:0]
    float curCpuDfsId = (float)((eax >> 8) & 0x3f);
    float curCpuFid = (float)(eax & 0xff);
    
    float clock = curCpuFid / curCpuDfsId * 200.0f;
    
//    PStateCur_perCore[physical] = curHwPstate;
    effFreq_perCore[physical] = clock;
    
    //    IOLog("AMDCPUSupport::updateClockSpeed: %u\n", curHwPstate);
}

void AMDRyzenCPUPowerManagement::calculateEffectiveFrequency(uint8_t physical){
    
    uint32_t APERF_lo, APERF_hi;
    uint32_t MPERF_lo, MPERF_hi;
    
    /**
     * The effective frequency interface provides +/- 50MHz accuracy if the following constraints are met:
     * • Effective frequency is read at most one time per millisecond.
     * • When reading or writing Core::X86::Msr::MPERF and Core::X86::Msr::APERF software executes only
     *  MOV instructions, and no more than 3 MOV instructions, between the two RDMSR or WRMSR
     *  instructions.
     * • Core::X86::Msr::MPERF and Core::X86::Msr::APERF are invalid if an overflow occurs.
    */
    __asm__ volatile("movl $0xe8, %%ecx;"
                     "rdmsr;"
                     "movl %%eax, %0;"
                     "movl %%edx, %1;"
                     "movl $0xe7, %%ecx;"
                     "rdmsr;"
                     : "=r"(APERF_lo), "=r"(APERF_hi), "=a"(MPERF_lo), "=d"(MPERF_hi)
                     :
                     : "%ecx"
                    );
    
    uint64_t APERF = APERF_lo | ((uint64_t)APERF_hi << 32);
    uint64_t MPERF = MPERF_lo | ((uint64_t)MPERF_hi << 32);;
        
    uint64_t lastAPERF = lastAPERF_PerCore[physical];
    uint64_t lastMPERF = lastMPERF_PerCore[physical];
    
    //If an overflow of either the MPERF or APERF register occurs between the read of last MPERF and the
    //read of last APERF, the effective frequency calculated in is invalid.
    //Yeah, so we will do nothing.
    if(APERF <= lastAPERF || MPERF <= lastMPERF) return;
    
    float freqP0 = PStateDefClock_perCore[0];
    
    uint64_t deltaAPERF = APERF - lastAPERF;
    deltaAPERF_PerCore[physical] = deltaAPERF;
    deltaMPERF_PerCore[physical] = MPERF - lastMPERF;
    float effFreq = ((float)deltaAPERF / (float)(MPERF - lastMPERF)) * freqP0;
    
    effFreq_perCore[physical] = effFreq;
    
    lastAPERF_PerCore[physical] = APERF;
    lastMPERF_PerCore[physical] = MPERF;
}

void AMDRyzenCPUPowerManagement::updateInstructionDelta(uint8_t cpu_num){
    uint64_t insCount;
    
    if(!read_msr(kMSR_PERF_IRPC, &insCount))
        panic("AMDCPUSupport::updateInstructionDelta: fucked up");
    
    
    //Skip if overflowed
    if(lastInstructionDelta_perCore[cpu_num] > insCount) return;
    
//    uint64_t delta = insCount - lastInstructionDelta_perCore[cpu_num];
    instructionDelta_PerCore[cpu_num] = insCount - lastInstructionDelta_perCore[cpu_num];
    
    lastInstructionDelta_perCore[cpu_num] = insCount;
    
    //write_msr(kMSR_PERF_IRPC, 0);
    
    
    //Calculate load index
//    float estimatedInstRet = (effFreq_perCore[cpu_num] * 1000000);
//    estimatedInstRet = estimatedInstRet * (actualUpdateTimeInterval * 0.001);
//    float index = (float)delta / estimatedInstRet;
//
//    float growth = 3200;
//    loadIndex_PerCore[cpu_num] = log10f(min(index,1) * growth) / log10f(growth);
}

void AMDRyzenCPUPowerManagement::applyPowerControl(){
    mp_rendezvous(nullptr, [](void *obj) {
        auto provider = static_cast<AMDRyzenCPUPowerManagement*>(obj);
        provider->write_msr(kMSR_PSTATE_CTL, (uint64_t)(provider->PStateCtl & 0x7));
    }, nullptr, this);
}

void AMDRyzenCPUPowerManagement::setCPBState(bool enabled){
    if(!cpbSupported) return;
    
    uint64_t hwConfig;
    if(!read_msr(kMSR_HWCR, &hwConfig))
        panic("AMDCPUSupport::setCPBState: wtf?");
    
    if(enabled){
        hwConfig &= ~(1 << 25);
    } else {
        hwConfig |= (1 << 25);
    }
    
    //A bit hacky but at least works for now.
    void* args[] = {this, &hwConfig};
    
    mp_rendezvous(nullptr, [](void *obj) {
        auto v = static_cast<uint64_t*>(*((uint64_t**)obj+1));
        auto provider = static_cast<AMDRyzenCPUPowerManagement*>(*((AMDRyzenCPUPowerManagement**)obj));
        provider->write_msr(kMSR_HWCR, *v);
    }, nullptr, args);
}

bool AMDRyzenCPUPowerManagement::getCPBState(){
    uint64_t hwConfig;
    if(!read_msr(kMSR_HWCR, &hwConfig))
        panic("AMDCPUSupport::start: wtf?");
    
    return !((hwConfig >> 25) & 0x1);
}

void AMDRyzenCPUPowerManagement::updatePackageTemp(){
    IOPCIAddressSpace space;
    space.bits = 0x00;
    
    fIOPCIDevice->configWrite32(space, (UInt8)kFAMILY_17H_PCI_CONTROL_REGISTER, (UInt32)kF17H_M01H_THM_TCON_CUR_TMP);
    uint32_t temperature = fIOPCIDevice->configRead32(space, kFAMILY_17H_PCI_CONTROL_REGISTER + 4);
    
    
    bool tempOffsetFlag = (temperature & kF17H_TEMP_OFFSET_FLAG) != 0;
    temperature = (temperature >> 21) * 125;
    
    float t = temperature * 0.001f;
    
    t -= tempOffset;
    
    if (tempOffsetFlag)
        t -= 49.0f;
    
    
    PACKAGE_TEMPERATURE_perPackage[0] = t;
    
    
//    IOPCIAddressSpace space2;
//    space2.bits = 0;
//    space2.es.deviceNum = 0x18;
//    space2.es.functionNum = 3;
//    uint32_t nn = fIOPCIDevice->configRead32(space2, 0x00);
//    uint64_t ca = 0;
//    read_msr(kMSR_CSTATE_ADDR, &ca);
//
//    IOLog("shit %u %llu\n", nn, ca);
}

void AMDRyzenCPUPowerManagement::updatePackageEnergy(){
    
    uint64_t ctsc = rdtsc64();

    uint64_t msr_value_buf = 0;
    read_msr(kMSR_PKG_ENERGY_STAT, &msr_value_buf);

    uint32_t enegryValue = (uint32_t)(msr_value_buf & 0xffffffff);

    uint64_t enegryDelta = (lastUpdateEnergyValue <= enegryValue) ?
    enegryValue - lastUpdateEnergyValue : UINT32_MAX - lastUpdateEnergyValue;

    double seconds = (ctsc - pwrLastTSC) / (double)(xnuTSCFreq);
    double e = (pwrEnegryUnit * enegryDelta) / (seconds);
    e *= pwrTimeUnit * 1000;
    uniPackageEnegry = e;


    lastUpdateEnergyValue = enegryValue;
    pwrLastTSC = rdtsc64();
}

void AMDRyzenCPUPowerManagement::dumpPstate(){
    
    uint8_t len = 0;
    for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
        uint64_t msr_value_buf = 0;
        bool err = !read_msr(kMSR_PSTATE_0 + i, &msr_value_buf);
        if(err) panic("AMDCPUSupport::dumpPstate: fucked up");
        
        uint32_t eax = (uint32_t)(msr_value_buf & 0xffffffff);
        
        // CpuVid [21:14]
        // CpuDfsId [13:8]
        // CpuFid [7:0]
        int curCpuDfsId = (int)((eax >> 8) & 0x3f);
        int curCpuFid = (int)(eax & 0xff);
        
        float clock = (float)((float)curCpuFid / (float)curCpuDfsId * 200.0);
        
        PStateDef_perCore[i] = msr_value_buf;
        PStateDefClock_perCore[i] = clock;
        
        if(msr_value_buf & ((uint64_t)1 << 63)) len++;
        //        IOLog("a: %llu", msr_value_buf);
    }
    
    PStateEnabledLen = max(PStateEnabledLen, len);
}

void AMDRyzenCPUPowerManagement::writePstate(const uint64_t *buf){
    
    PStateEnabledLen = 0;
    
    //A bit hacky but at least works for now.
    void* args[] = {this, (void*)buf};
    
    
    mp_rendezvous(nullptr, [](void *obj) {
        auto v = static_cast<uint64_t*>(((uint64_t**)obj)[1]);
        auto provider = static_cast<AMDRyzenCPUPowerManagement*>(*((AMDRyzenCPUPowerManagement**)obj));

        for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
            uint64_t def = v[i];
            
            uint64_t curCpuDfsId = ((def >> 8) & 0x3f);
            uint64_t curCpuFid = (def & 0xff);
            if(!def || !curCpuDfsId || !curCpuFid)
                continue;
            
            provider->write_msr(provider->kMSR_PSTATE_0 + i, def);
            
        }
    
        
        if(!pmRyzen_cpu_is_master(cpu_number())) return;
        provider->dumpPstate();
        
    }, nullptr, args);
        
}

uint32_t AMDRyzenCPUPowerManagement::getPMPStateLimit(){
    return pmRyzen_pstatelimit;
}

void AMDRyzenCPUPowerManagement::setPMPStateLimit(uint32_t state){
    pmRyzen_pstatelimit = min(2, state);
    if(state > 0){
        pmRyzen_PState_reset();
    }
}

uint32_t AMDRyzenCPUPowerManagement::getHPcpus(){
    return pmRyzen_hpcpus;
}

EXPORT extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t *, void *) {
    // Report success but actually do not start and let I/O Kit unload us.
    // This works better and increases boot speed in some cases.
    PE_parse_boot_argn("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
    ADDPR(debugEnabled) = checkKernelArgument("-amdpdbg");
    return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t *, void *) {
    // It is not safe to unload VirtualSMC plugins!
    return KERN_FAILURE;
}


#ifdef __MAC_10_15

// macOS 10.15 adds Dispatch function to all OSObject instances and basically
// every header is now incompatible with 10.14 and earlier.
// Here we add a stub to permit older macOS versions to link.
// Note, this is done in both kern_util and plugin_start as plugins will not link
// to Lilu weak exports from vtable.

kern_return_t WEAKFUNC PRIVATE OSObject::Dispatch(const IORPC rpc) {
    PANIC("util", "OSObject::Dispatch smcproc stub called");
}

kern_return_t WEAKFUNC PRIVATE OSMetaClassBase::Dispatch(const IORPC rpc) {
    PANIC("util", "OSMetaClassBase::Dispatch smcproc stub called");
}

#endif
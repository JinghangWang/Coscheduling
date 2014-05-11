#include <dev/apic.h>
#include <cpuid.h>
#include <msr.h>
#include <paging.h>
#include <nautilus.h>


#ifndef NAUT_CONFIG_DEBUG_APIC
#undef DEBUG_PRINT
#define DEBUG_PRINT(fmt, args...)
#endif


int 
check_apic_avail (void)
{
    cpuid_ret_t cp;
    struct cpuid_feature_flags * flags;

    cp    = cpuid(CPUID_FEATURE_INFO);
    flags = (struct cpuid_feature_flags *)&cp.c;

    return flags->edx.apic;
}


static int 
apic_is_bsp (struct apic_dev * apic)
{
    uint64_t data;
    data = msr_read(IA32_APIC_BASE_MSR);
    return APIC_IS_BSP(data);
}


static int
apic_sw_enable (struct apic_dev * apic)
{
    uint32_t val;
    // KCH: TODO fix this
    //apic_write(APIC_REG_LVT0, 0);
    
    val = apic_read(apic, APIC_REG_SPIV);
    apic_write(apic, APIC_REG_SPIV, val | APIC_SPIV_SW_ENABLE);
    return 0;
}


static int
apic_sw_disable (struct apic_dev * apic)
{
    uint32_t val;
    val = apic_read(apic, APIC_REG_SPIV);
    apic_write(apic, APIC_REG_SPIV, val & ~APIC_SPIV_SW_ENABLE);
    return 0;
}


static void 
apic_enable (struct apic_dev * apic) 
{
    uint64_t data;
    data = msr_read(IA32_APIC_BASE_MSR);
    msr_write(IA32_APIC_BASE_MSR, data | APIC_GLOBAL_ENABLE);
    apic_sw_enable(apic);
}


static ulong_t 
apic_get_base_addr (void) 
{
    uint64_t data;
    data = msr_read(IA32_APIC_BASE_MSR);

    // we're assuming PAE is on
    return (addr_t)(data & APIC_BASE_ADDR_MASK);
}


static void
apic_set_base_addr (struct apic_dev * apic, addr_t addr)
{
    uint64_t data;
    data = msr_read(IA32_APIC_BASE_MSR);
    
    msr_write(IA32_APIC_BASE_MSR, (addr & APIC_BASE_ADDR_MASK) | (data & 0xfff));
}


void 
apic_do_eoi (struct apic_dev * apic)
{
    apic_write(apic, APIC_REG_EOR, 0);
}


static uint32_t
apic_get_id (struct apic_dev * apic)
{
    return apic_read(apic, APIC_REG_ID) >> APIC_ID_SHIFT;
}


static inline uint8_t 
apic_get_version (struct apic_dev * apic)
{
    return APIC_VERSION(apic_read(apic, APIC_REG_LVR));
}


void 
apic_ipi (struct apic_dev * apic, 
          uint_t remote_id,
          uint_t vector)
{
    apic_write(apic, APIC_REG_ICR2, remote_id << APIC_ICR2_DST_SHIFT);
    apic_write(apic, APIC_REG_ICR, vector | APIC_ICR_LEVEL_ASSERT);
}


void
apic_self_ipi (struct apic_dev * apic, uint_t vector)
{
    apic_write(apic, APIC_IPI_SELF | APIC_ICR_TYPE_FIXED, vector);
}


void
apic_init (struct apic_dev * apic)
{

    if (!check_apic_avail()) {
        panic("no APIC found, dying\n");
    } 

    apic->base_addr = apic_get_base_addr();
    DEBUG_PRINT("apic base addr: %p\n", apic->base_addr);

    DEBUG_PRINT("Reserving APIC region\n");

    if (reserve_page(apic->base_addr) < 0) {
        panic("Couldn't reserve LAPIC mem region\n");
    }
    
    /* map in the lapic as uncacheable */
    create_page_mapping(apic->base_addr, 
                        apic->base_addr, 
                        PTE_PRESENT_BIT|PTE_WRITABLE_BIT|PTE_CACHE_DISABLE_BIT);

    apic->version   = apic_get_version(apic);
    apic->id        = apic_get_id(apic);

    DEBUG_PRINT("Found LAPIC (version=0x%x, id=0x%x)\n", apic->version, apic->id);

    if (apic->version < 0x10 || apic->version > 0x15) {
        panic("Unsupported APIC version (0x%1x)\n", (unsigned)apic->version);
    }

    apic_enable(apic);
}

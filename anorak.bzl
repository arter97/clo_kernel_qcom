load(":image_opts.bzl", "boot_image_opts")
load(":msm_kernel_la.bzl", "define_msm_la")
load(":target_variants.bzl", "la_variants")

target_name = "anorak"

def define_anorak():
    _anorak_in_tree_modules = [
        # keep sorted
        "drivers/dma-buf/heaps/qcom_dma_heaps.ko",
        "drivers/edac/kryo_arm64_edac.ko",
        "drivers/edac/qcom_edac.ko",
        "drivers/firmware/arm_scmi/qcom_scmi_vendor.ko",
        "drivers/firmware/qcom-scm.ko",
        "drivers/hwspinlock/qcom_hwspinlock.ko",
        "drivers/hwtracing/stm/stm_console.ko",
        "drivers/hwtracing/stm/stm_core.ko",
        "drivers/hwtracing/stm/stm_ftrace.ko",
        "drivers/hwtracing/stm/stm_p_ost.ko",
        "drivers/iommu/arm/arm-smmu/arm_smmu.ko",
        "drivers/iommu/iommu-logger.ko",
        "drivers/iommu/msm_dma_iommu_mapping.ko",
        "drivers/iommu/qcom_iommu_debug.ko",
        "drivers/iommu/qcom_iommu_util.ko",
        "drivers/irqchip/msm_show_resume_irq.ko",
        "drivers/nvmem/nvmem_qfprom.ko",
        "drivers/perf/qcom_llcc_pmu.ko",
        "drivers/pinctrl/qcom/pinctrl-msm.ko",
        "drivers/power/reset/qcom-dload-mode.ko",
        "drivers/power/reset/qcom-reboot-reason.ko",
        "drivers/remoteproc/qcom_pil_info.ko",
        "drivers/remoteproc/qcom_q6v5.ko",
        "drivers/remoteproc/qcom_q6v5_pas.ko",
        "drivers/remoteproc/qcom_spss.ko",
        "drivers/remoteproc/qcom_sysmon.ko",
        "drivers/remoteproc/rproc_qcom_common.ko",
        "drivers/rpmsg/qcom_glink.ko",
        "drivers/rpmsg/qcom_glink_smem.ko",
        "drivers/rpmsg/qcom_glink_spss.ko",
        "drivers/rpmsg/qcom_smd.ko",
        "drivers/soc/qcom/boot_stats.ko",
        "drivers/soc/qcom/cmd-db.ko",
        "drivers/soc/qcom/core_hang_detect.ko",
        "drivers/soc/qcom/dcvs/bwmon.ko",
        "drivers/soc/qcom/dcvs/cpufreq_stats_scmi_v2.ko",
        "drivers/soc/qcom/dcvs/dcvs_fp.ko",
        "drivers/soc/qcom/dcvs/memlat.ko",
        "drivers/soc/qcom/dcvs/qcom-dcvs.ko",
        "drivers/soc/qcom/dcvs/qcom-pmu-lib.ko",
        "drivers/soc/qcom/dcvs/qcom_scmi_client.ko",
        "drivers/soc/qcom/debug_symbol.ko",
        "drivers/soc/qcom/eud.ko",
        "drivers/soc/qcom/llcc-qcom.ko",
        "drivers/soc/qcom/llcc_perfmon.ko",
        "drivers/soc/qcom/mdt_loader.ko",
        "drivers/soc/qcom/mem-hooks.ko",
        "drivers/soc/qcom/mem-offline.ko",
        "drivers/soc/qcom/mem_buf/mem_buf.ko",
        "drivers/soc/qcom/mem_buf/mem_buf_dev.ko",
        "drivers/soc/qcom/memory_dump_v2.ko",
        "drivers/soc/qcom/minidump.ko",
        "drivers/soc/qcom/msm_performance.ko",
        "drivers/soc/qcom/pdr_interface.ko",
        "drivers/soc/qcom/qcom_cpu_vendor_hooks.ko",
        "drivers/soc/qcom/qcom_cpucp.ko",
        "drivers/soc/qcom/qcom_logbuf_vendor_hooks.ko",
        "drivers/soc/qcom/qcom_ramdump.ko",
        "drivers/soc/qcom/qcom_rpmh.ko",
        "drivers/soc/qcom/qcom_va_minidump.ko",
        "drivers/soc/qcom/qcom_wdt_core.ko",
        "drivers/soc/qcom/qfprom-sys.ko",
        "drivers/soc/qcom/qmi_helpers.ko",
        "drivers/soc/qcom/qsee_ipc_irq_bridge.ko",
        "drivers/soc/qcom/rq_stats.ko",
        "drivers/soc/qcom/secure_buffer.ko",
        "drivers/soc/qcom/smem.ko",
        "drivers/soc/qcom/socinfo.ko",
        "drivers/soc/qcom/sysmon_subsystem_stats.ko",
        "drivers/thermal/qcom/qti_cpufreq_cdev.ko",
        "drivers/tty/serial/msm_geni_serial.ko",
        "kernel/msm_sysstats.ko",
        "kernel/sched/walt/sched-walt.ko",
    ]

    _anorak_consolidate_in_tree_modules = _anorak_in_tree_modules + [
        # keep sorted
        "drivers/misc/lkdtm/lkdtm.ko",
        "kernel/sched/walt/sched-walt-debug.ko",
    ]

    kernel_vendor_cmdline_extras = [
        # do not sort
        "console=ttyMSM0,115200n8",
        "qcom_geni_serial.con_enabled=1",
        "bootconfig",
    ]

    for variant in la_variants:
        board_kernel_cmdline_extras = []
        board_bootconfig_extras = []

        if variant == "consolidate":
            mod_list = _anorak_consolidate_in_tree_modules
        else:
            mod_list = _anorak_in_tree_modules
            board_kernel_cmdline_extras += ["nosoftlockup"]
            kernel_vendor_cmdline_extras += ["nosoftlockup"]
            board_bootconfig_extras += ["androidboot.console=0"]

        define_msm_la(
            msm_target = target_name,
            variant = variant,
            in_tree_module_list = mod_list,
            boot_image_opts = boot_image_opts(
                earlycon_addr = "qcom_geni,0x00998000",
                kernel_vendor_cmdline_extras = kernel_vendor_cmdline_extras,
                board_kernel_cmdline_extras = board_kernel_cmdline_extras,
                board_bootconfig_extras = board_bootconfig_extras,
            ),
            #TODO: Need to enable this
            #dpm_overlay = True,
        )

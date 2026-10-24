// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Lumia 950 (MSM8992) board constants for the display path.
//
// Mainline Linux does not describe this SoC's display either: msm8994.dtsi has
// no mdss node, no dsi node and no mdp node. The values below come from
// Qualcomm's msm-3.10 device tree and register headers for the same SoC,
// published for the LG G4 (which carries an MSM8992 and this generation of
// MDSS):
//
//   LineageOS/android_kernel_lge_msm8992, branch lineage-15.1
//     arch/arm/boot/dts/qcom/msm8992-mdss.dtsi
//     arch/arm/boot/dts/qcom/msm-gdsc.dtsi
//     drivers/video/msm/mdss/dsi_host_v2.h
//     drivers/video/msm/mdss/mdss_mdp_hwio.h
//
//   mdss_mdp: qcom,mdss_mdp@fd900000 {
//       compatible = "qcom,mdss_mdp";
//       reg = <0xfd900000 0x90000>,          // mdp_phys
//             <0xfd9b0000 0x004d4>;          // vbif_phys
//       interrupts = <0 83 0>;               // GIC SPI 83
//       vdd-supply = <&gdsc_mdss>;
//       qcom,mdss-mdp-reg-offset = <0x00001000>;
//   };
//
//   mdss_dsi0: qcom,mdss_dsi@fd994000 {
//       reg = <0xfd994000 0x260>,            // dsi_ctrl
//             <0xfd994500 0x280>,            // dsi_phy
//             <0xfd994780 0x030>,            // dsi_phy_regulator
//             <0xfd828000 0x108>;            // mmss_misc_phys
//       gdsc-supply = <&gdsc_mdss>;
//       vdd-supply    = <&pm8994_l14>;       // 1.8 V
//       vddio-supply  = <&pm8994_l12>;       // 1.8 V
//       vdda-supply   = <&pm8994_l2>;        // 1.2 V
//       vcca-supply   = <&pm8994_l28>;       // 2.85 V
//       clocks = ... clk_mdss_byte0_clk, clk_mdss_pclk0_clk, clk_mdss_esc0_clk ...
//   };
//
//   gdsc_mdss: qcom,gdsc@fd8c2304 { reg = <0xfd8c2304 0x4>; };
//
// The GDSC's one readable state bit is PWR_ON (bit 31), the same bit the GPU's
// two OxiLi domains report, and the same bit drivers/clk/qcom/gdsc.c polls.
//
// What is deliberately NOT here:
//
//   * The DSI PHY's PLL register values. The PHY is a 28 nm block whose
//     programming (VCO, post-dividers, timing, LDO, calibration) is a table of
//     magic numbers per link rate in Qualcomm's mdss_dsi_phy code; none of it is
//     in this repository, so no value here pretends to be.
//   * The panel's own init sequence. The Lumia 950's panel is not the LG G4's,
//     so the G4's panel DTSI is not a source for it.
//   * The DSI video timing the firmware actually programmed. Rung 1 reads those
//     registers and logs them; it does not claim to know their field layout.
//
// Rung 1 only reads: the windows below are mapped, the domain is checked, and
// the version and status registers are logged. Nothing is written.
#ifndef NATIVE_MDSS_BOARD_CONSTANTS_H
#define NATIVE_MDSS_BOARD_CONSTANTS_H

#include "MdssTypes.h"

namespace mdss {

// MDP window, "mdp_phys" in the device tree node.
constexpr u64 kMdpPhys = 0xfd900000ULL;
constexpr u32 kMdpSize = 0x90000;

// The register block inside that window: qcom,mdss-mdp-reg-offset.
constexpr u32 kMdpRegisterOffset = 0x1000;

// VBIF, "vbif_phys": the bus interface the MDP fetches through.
constexpr u64 kVbifPhys = 0xfd9b0000ULL;
constexpr u32 kVbifSize = 0x4d4;

// DSI controller 0. The Lumia 950's panel is on the primary DSI link.
constexpr u64 kDsi0Phys = 0xfd994000ULL;
constexpr u32 kDsi0Size = 0x260;

// DSI PHY for that link. Rung 1 maps it and reads nothing: its registers are
// addressed by an encoding this repository does not have, and a wrong read in a
// PHY window is a wrong answer, not a harmless one.
constexpr u64 kDsiPhy0Phys = 0xfd994500ULL;
constexpr u32 kDsiPhy0Size = 0x280;

// MMSS misc block, "mmss_misc_phys": the ULP clamp and PHY reset controls.
constexpr u64 kMmssMiscPhys = 0xfd828000ULL;
constexpr u32 kMmssMiscSize = 0x108;

// MMSS clock controller. The GPU's constants file already maps it for the OxiLi
// domains; the display's own domain lives in the same 64 KiB block, and the
// window is sized to reach it.
constexpr u64 kMmssPhys = 0xfd8c0000ULL;
constexpr u32 kMmssSize = 0x10000;
constexpr u32 kGdscrMdssOffset = 0x2304;   // gdsc_mdss

// drivers/clk/qcom/gdsc.c: the domain is on when this bit reads 1.
constexpr u32 kGdscrPowerOn = 1u << 31;
constexpr u32 kGdscrSwCollapse = 1u << 0;
constexpr u32 kGdscrHwControl = 1u << 1;

// GIC SPI 83, the mdss_mdp interrupt of the device tree node. Nothing enables
// it in this rung; the number is here so the ADT node and the driver agree.
constexpr u32 kMdssIrq = 83;

// The panel this board has, from the firmware's GOP mode (README.md,
// "Resolution and screenshots") and scripts/panel.py.
constexpr u32 kPanelWidth = 1440;
constexpr u32 kPanelHeight = 2560;

// MDSS top-level and MDP registers (mdss_mdp_hwio.h). The first is relative to
// the MDSS base, the second to the MDSS base plus kMdpRegisterOffset.
constexpr u32 kRegMdssHwVersion = 0x0000;
constexpr u32 kRegMdpHwVersion = 0x0000;
constexpr u32 kRegMdpDispIntfSel = 0x0004;
constexpr u32 kRegMdpIntrStatus = 0x0014;

// DSI controller registers (dsi_host_v2.h), byte offsets in the ctrl window.
// Only the ones whose field meaning that header states are used for anything
// but logging.
constexpr u32 kRegDsiCtrl = 0x0000;
constexpr u32 kRegDsiStatus = 0x0004;
constexpr u32 kRegDsiVideoModeCtrl = 0x000c;
constexpr u32 kRegDsiVideoModeActiveH = 0x0020;
constexpr u32 kRegDsiVideoModeActiveV = 0x0024;
constexpr u32 kRegDsiVideoModeTotal = 0x0028;
constexpr u32 kRegDsiVideoModeHsync = 0x002c;
constexpr u32 kRegDsiVideoModeVsync = 0x0030;
constexpr u32 kRegDsiClkCtrl = 0x0118;
constexpr u32 kRegDsiClkStatus = 0x011c;
constexpr u32 kRegDsiVersion = 0x01f0;

// dsi_host_v2.h: DSI_CTRL's three enable bits.
constexpr u32 kDsiCtrlEnable = 1u << 0;
constexpr u32 kDsiCtrlVideoMode = 1u << 1;
constexpr u32 kDsiCtrlCommandMode = 1u << 2;

} // namespace mdss

#endif // NATIVE_MDSS_BOARD_CONSTANTS_H

/*
 * Copyright 2018 NXP
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <hwconfig.h>
#include <command.h>
#include <netdev.h>
#include <malloc.h>
#include <fsl_mdio.h>
#include <miiphy.h>
#include <phy.h>
#include <fm_eth.h>
#include <asm/io.h>
#include <exports.h>
#include <asm/arch/fsl_serdes.h>
#include <fsl-mc/fsl_mc.h>
#include <fsl-mc/ldpaa_wriop.h>

#include "../common/qixis.h"

DECLARE_GLOBAL_DATA_PTR;

#define EMI_NONE	0
#define EMI1		1 /* Mdio Bus 1 */
#define EMI2		2 /* Mdio Bus 2 */

#if defined(CONFIG_FSL_MC_ENET) && !defined(CONFIG_SPL_BUILD)
typedef enum {
	IO_SLOT_NONE = 0,
	IO_SLOT_1,
	IO_SLOT_2,
	IO_SLOT_3,
	IO_SLOT_4,
	IO_SLOT_5,
	IO_SLOT_6,
	IO_SLOT_8,
	EMI1_RGMII1,
	EMI1_RGMII2,
	IO_SLOT_MAX
} IO_SLOT;

/* structure explaining the phy configuration on 8 lanes of a serdes*/
struct serdes_phy_config {
	u8 serdes; /* serdes protocol */
	struct phy_config {
		u8 dpmacid;
		u8 phy_address;
		u8 mdio_bus;
		IO_SLOT ioslot;
	} phy_config[SRDS_MAX_LANES];
};

/* Table defining the phy configuration on 8 lanes of a serdes.
 * Various assumptions have been made while defining this table.
 * e.g. for serdes1 protocol 0x2A it is being assummed that X-M11-USXGMII
 * card is being used for dpmac 5-8. (X-M12-XFI could also have been used)
 * And also that this card is connected to IO Slot 3 (could have been connected
 * to any of the 8 IO slots (IO slot 1 - IO slot 8)).
 * To override these settings "dpmac" environment variable can be used after
 * defining "dpmac_override" in hwconfig environment variable.
 * This table has limited serdes protocol entries. It can be expanded as per
 * requirement.
 */
static struct serdes_phy_config serdes1_phy_config[] = {
	{3, {{WRIOP1_DPMAC3, AQ_PHY_ADDR1, EMI1, IO_SLOT_1},
	    {WRIOP1_DPMAC4, AQ_PHY_ADDR2, EMI1, IO_SLOT_1},
	    {WRIOP1_DPMAC5, AQ_PHY_ADDR3, EMI1, IO_SLOT_1},
	    {WRIOP1_DPMAC6, AQ_PHY_ADDR4, EMI1, IO_SLOT_1} } },
	{7, {{WRIOP1_DPMAC3, AQ_PHY_ADDR1, EMI1, IO_SLOT_1},
	    {WRIOP1_DPMAC4, AQ_PHY_ADDR2, EMI1, IO_SLOT_1},
	    {WRIOP1_DPMAC5, AQ_PHY_ADDR3, EMI1, IO_SLOT_1},
	    {WRIOP1_DPMAC6, AQ_PHY_ADDR4, EMI1, IO_SLOT_1},
	    {WRIOP1_DPMAC7, SGMII_CARD_PORT1_PHY_ADDR, EMI1, IO_SLOT_2},
	    {WRIOP1_DPMAC8, SGMII_CARD_PORT2_PHY_ADDR, EMI1, IO_SLOT_2},
	    {WRIOP1_DPMAC9, SGMII_CARD_PORT3_PHY_ADDR, EMI1, IO_SLOT_2},
	    {WRIOP1_DPMAC10, SGMII_CARD_PORT4_PHY_ADDR, EMI1, IO_SLOT_2} } },
	{13, {{WRIOP1_DPMAC1, INPHI_PHY_ADDR1, EMI1, IO_SLOT_1},
	     {WRIOP1_DPMAC2, INPHI_PHY_ADDR1, EMI1, IO_SLOT_2} } },
	{15, {{WRIOP1_DPMAC1, INPHI_PHY_ADDR1, EMI1, IO_SLOT_1},
	     {WRIOP1_DPMAC2, INPHI_PHY_ADDR1, EMI1, IO_SLOT_1} } },
	{19, {{WRIOP1_DPMAC2, CORTINA_PHY_ADDR1, EMI1, IO_SLOT_2},
	     {WRIOP1_DPMAC3, AQ_PHY_ADDR1, EMI1, IO_SLOT_1},
	     {WRIOP1_DPMAC4, AQ_PHY_ADDR2, EMI1, IO_SLOT_1},
	     {WRIOP1_DPMAC5, INPHI_PHY_ADDR1, EMI1, IO_SLOT_6},
	     {WRIOP1_DPMAC6, INPHI_PHY_ADDR1, EMI1, IO_SLOT_6} } }
};

static struct serdes_phy_config serdes2_phy_config[] = {
	{2, {} },
	{3, {} },
	{5, {} }
};

static struct serdes_phy_config serdes3_phy_config[] = {
	{2, {} }
};

static inline struct phy_config *get_phy_config(u8 serdes,
						struct serdes_phy_config *table,
						u8 table_size)
{
	int i;

	for (i = 0; i < table_size; i++) {
		if (table[i].serdes == serdes)
			return table[i].phy_config;
	}

	return NULL;
}

struct lx2160a_qds_mdio {
	IO_SLOT ioslot:4;
	u8 realbusnum:4;
	struct mii_dev *realbus;
};

/* BRDCFG4 controls EMI routing for the board.
 * Bits    Function
 * 7-6     EMI Interface #1 Primary Routing (CFG_MUX1_EMI1) (1.8V):
 * EMI1    00= On-board PHY #1
 *         01= On-board PHY #2
 *         10= (reserved)
 *         11= Slots 1..8 multiplexer and translator.
 * 5-3     EMI Interface #1 Secondary Routing (CFG_MUX2_EMI1) (2.5V):
 * EMI1X   000= Slot #1
 *         001= Slot #2
 *         010= Slot #3
 *         011= Slot #4
 *         100= Slot #5
 *         101= Slot #6
 *         110= Slot #7
 *         111= Slot #8
 * 2-0     EMI Interface #2 Routing (CFG_MUX_EMI2):
 * EMI2    000= Slot #1 (secondary EMI)
 *         001= Slot #2 (secondary EMI)
 *         010= Slot #3 (secondary EMI)
 *         011= Slot #4 (secondary EMI)
 *         100= Slot #5 (secondary EMI)
 *         101= Slot #6 (secondary EMI)
 *         110= Slot #7 (secondary EMI)
 *         111= Slot #8 (secondary EMI)
 */
static void lx2160a_qds_mux_mdio(struct lx2160a_qds_mdio *priv)
{
	u8 brdcfg4, reg;

	brdcfg4 = QIXIS_READ(brdcfg[4]);
	reg = brdcfg4;

	switch (priv->realbusnum) {
	case EMI1:
		switch (priv->ioslot) {
		case EMI1_RGMII1:
			brdcfg4 &= ~0xC0;
			break;
		case EMI1_RGMII2:
			brdcfg4 &= ~0xC0;
			brdcfg4 |= 1U << 6;
			break;
		default:
			brdcfg4 |= 3U << 6;
			brdcfg4 &= ~BRDCFG4_EMI1SEL_MASK;
			brdcfg4 |= (priv->ioslot - 1) << BRDCFG4_EMI1SEL_SHIFT;
			break;
		}
		break;
	case EMI2:
		brdcfg4 &= ~BRDCFG4_EMI2SEL_MASK;
		brdcfg4 |= (priv->ioslot - 1) << BRDCFG4_EMI2SEL_SHIFT;
		break;
	}

	if (brdcfg4 ^ reg)
		QIXIS_WRITE(brdcfg[4], brdcfg4);
}

static int lx2160a_qds_mdio_read(struct mii_dev *bus, int addr,
				 int devad, int regnum)
{
	struct lx2160a_qds_mdio *priv = bus->priv;

	lx2160a_qds_mux_mdio(priv);

	return priv->realbus->read(priv->realbus, addr, devad, regnum);
}

static int lx2160a_qds_mdio_write(struct mii_dev *bus, int addr, int devad,
				  int regnum, u16 value)
{
	struct lx2160a_qds_mdio *priv = bus->priv;

	lx2160a_qds_mux_mdio(priv);

	return priv->realbus->write(priv->realbus, addr, devad, regnum, value);
}

static int lx2160a_qds_mdio_reset(struct mii_dev *bus)
{
	struct lx2160a_qds_mdio *priv = bus->priv;

	return priv->realbus->reset(priv->realbus);
}

static struct mii_dev *lx2160a_qds_mdio_init(u8 realbusnum, IO_SLOT ioslot)
{
	struct lx2160a_qds_mdio *pmdio;
	struct mii_dev *bus;
	/*should be within MDIO_NAME_LEN*/
	char dummy_mdio_name[] = "LX2160A_QDS_MDIO1_IOSLOT1";

	if (realbusnum == EMI2) {
		if ((ioslot < IO_SLOT_1) || (ioslot > IO_SLOT_8)) {
			printf("invalid ioslot %d\n", ioslot);
			return NULL;
		}
	} else if (realbusnum == EMI1) {
		if ((ioslot < IO_SLOT_1) || (ioslot > EMI1_RGMII2)) {
			printf("invalid ioslot %d\n", ioslot);
			return NULL;
		}
	} else {
		printf("not supported real mdio bus %d\n", realbusnum);
		return NULL;
	}

	if (ioslot == EMI1_RGMII1)
		strcpy(dummy_mdio_name, "LX2160A_QDS_MDIO1_RGMII1");
	else if (ioslot == EMI1_RGMII2)
		strcpy(dummy_mdio_name, "LX2160A_QDS_MDIO1_RGMII2");
	else
		sprintf(dummy_mdio_name, "LX2160A_QDS_MDIO%d_IOSLOT%d",
			realbusnum, ioslot);
	bus = miiphy_get_dev_by_name(dummy_mdio_name);

	if (bus)
		return bus;

	bus = mdio_alloc();
	if (!bus) {
		printf("Failed to allocate %s bus\n", dummy_mdio_name);
		return NULL;
	}

	pmdio = malloc(sizeof(*pmdio));
	if (!pmdio) {
		printf("Failed to allocate %s private data\n", dummy_mdio_name);
		free(bus);
		return NULL;
	}

	switch (realbusnum) {
	case EMI1:
		pmdio->realbus = miiphy_get_dev_by_name(DEFAULT_WRIOP_MDIO1_NAME);
		break;
	case EMI2:
		pmdio->realbus = miiphy_get_dev_by_name(DEFAULT_WRIOP_MDIO2_NAME);
		break;
	}

	if (!pmdio->realbus) {
		printf("No real mdio bus num %d found\n", realbusnum);
		free(bus);
		free(pmdio);
		return NULL;
	}

	pmdio->realbusnum = realbusnum;
	pmdio->ioslot = ioslot;
	bus->read = lx2160a_qds_mdio_read;
	bus->write = lx2160a_qds_mdio_write;
	bus->reset = lx2160a_qds_mdio_reset;
	strcpy(bus->name, dummy_mdio_name);
	bus->priv = pmdio;

	if (!mdio_register(bus))
		return bus;

	printf("No bus with name %s\n", dummy_mdio_name);
	free(bus);
	free(pmdio);
	return NULL;
}

static inline void do_phy_config(struct phy_config *phy_config)
{
	struct mii_dev *bus;
	int i;

	for (i = 0; i < SRDS_MAX_LANES; i++) {
		if (phy_config[i].dpmacid) {
			wriop_set_phy_address(phy_config[i].dpmacid,
					      phy_config[i].phy_address);
			/*Register the muxing front-ends to the MDIO buses*/
			bus = lx2160a_qds_mdio_init(phy_config[i].mdio_bus,
						    phy_config[i].ioslot);
			if (!bus)
				printf("could not get bus for mdio %d"
				       " ioslot %d\n",	phy_config[i].mdio_bus,
				       phy_config[i].ioslot);
			else
				wriop_set_mdio(phy_config[i].dpmacid, bus);
		}
	}
}

static inline void do_dpmac_config(int dpmac, const char *arg_dpmacid,
				   char *env_dpmac)
{
	const char *ret;
	size_t len;
	u8 realbusnum, ioslot;
	struct mii_dev *bus;

	/*search phy in dpmac arg*/
	ret = hwconfig_subarg_f(arg_dpmacid, "phy", &len, env_dpmac);
	if (ret) {
		if ((len != 4) ||
		    (strncmp(ret, "0x", 2)))
			printf("please specify phy for %s in 0x[XX] format in "
			       "%s variable. e.g. 0x12\n", arg_dpmacid,
			       env_dpmac);
		else
			wriop_set_phy_address(dpmac,
					      simple_strtoul(ret, NULL, 16));
	}
	/*search mdio in dpmac arg*/
	ret = hwconfig_subarg_f(arg_dpmacid, "mdio", &len, env_dpmac);
	if (ret)
		realbusnum = *ret - '0';
	else
		realbusnum = EMI_NONE;

	if (realbusnum) {
		/*search io in dpmac arg*/
		ret = hwconfig_subarg_f(arg_dpmacid, "io", &len, env_dpmac);
		if (ret)
			ioslot = *ret - '0';
		else
			ioslot = IO_SLOT_NONE;
		/*Register the muxing front-ends to the MDIO buses*/
		bus = lx2160a_qds_mdio_init(realbusnum, ioslot);
		if (!bus)
			printf("could not get bus for mdio %d ioslot %d\n",
			       realbusnum, ioslot);
		else
			wriop_set_mdio(dpmac, bus);
	}
}

#endif

int board_eth_init(bd_t *bis)
{
#ifndef CONFIG_ARCH_LX2160A_EMU_COMMON
#if defined(CONFIG_FSL_MC_ENET) && !defined(CONFIG_SPL_BUILD)
	struct memac_mdio_info mdio_info;
	struct memac_mdio_controller *regs;
	int i;
	u8 ioslot;
	const char *ret;
	char *env_dpmac;
	char dpmacid[] = "dpmac00", srds[] = "00_00_00";
	size_t len;
	struct mii_dev *bus;
	struct phy_config *phy_config;
	struct ccsr_gur *gur = (void *)(CONFIG_SYS_FSL_GUTS_ADDR);
	u32 srds_s1, srds_s2, srds_s3;

	srds_s1 = in_le32(&gur->rcwsr[28]) &
				FSL_CHASSIS3_RCWSR28_SRDS1_PRTCL_MASK;
	srds_s1 >>= FSL_CHASSIS3_RCWSR28_SRDS1_PRTCL_SHIFT;

	srds_s2 = in_le32(&gur->rcwsr[28]) &
				FSL_CHASSIS3_RCWSR28_SRDS2_PRTCL_MASK;
	srds_s2 >>= FSL_CHASSIS3_RCWSR28_SRDS2_PRTCL_SHIFT;

	srds_s3 = in_le32(&gur->rcwsr[28]) &
				FSL_CHASSIS3_RCWSR28_SRDS3_PRTCL_MASK;
	srds_s3 >>= FSL_CHASSIS3_RCWSR28_SRDS3_PRTCL_SHIFT;

	sprintf(srds, "%02d_%02d_%02d", srds_s1, srds_s2, srds_s3);

	regs = (struct memac_mdio_controller *)CONFIG_SYS_FSL_WRIOP1_MDIO1;
	mdio_info.regs = regs;
	mdio_info.name = DEFAULT_WRIOP_MDIO1_NAME;

	/*Register the EMI 1*/
	fm_memac_mdio_init(bis, &mdio_info);

	regs = (struct memac_mdio_controller *)CONFIG_SYS_FSL_WRIOP1_MDIO2;
	mdio_info.regs = regs;
	mdio_info.name = DEFAULT_WRIOP_MDIO2_NAME;

	/*Register the EMI 2*/
	fm_memac_mdio_init(bis, &mdio_info);

	/* "dpmac" environment variable can be used after
	 * defining "dpmac_override" in hwconfig environment variable.
	 */
	if (hwconfig("dpmac_override")) {
		env_dpmac = getenv("dpmac");
		if (env_dpmac) {
			ret = hwconfig_arg_f("srds", &len, env_dpmac);
			if (ret) {
				if (strncmp(ret, srds, strlen(srds))) {
					printf("Warning: SERDES configuration "
					       " changed, previous: %.*s, "
					       "current: %s. consider updating "
					       "dpmac environment variable\n",
					       (int)len, ret, srds);
				}
			} else {
				printf("Warning: SERDES configuration not found"
				       " in dpmac environment variable. Please "
				       "add srds:%s in dpmac environment "
				       "variable\n", srds);
			}

			for (i = WRIOP1_DPMAC1; i <= WRIOP1_DPMAC16; i++) {
				/* Look for dpmac1..dpmac16 arg in dpmac
				 * environment variable
				 */
				sprintf(dpmacid, "dpmac%d", i);
				ret = hwconfig_arg_f(dpmacid, &len, env_dpmac);
				if (ret)
					do_dpmac_config(i, dpmacid, env_dpmac);
			}
		} else {
			printf("Warning: environment dpmac not found, "
				"DPAA network interfaces may not work\n");
		}
	} else {
		/*Look for phy config for serdes1 in phy config table*/
		phy_config = get_phy_config(srds_s1,
					    serdes1_phy_config,
					    ARRAY_SIZE(serdes1_phy_config));
		if (!phy_config) {
			printf("%s qds: WRIOP: Unsupported SerDes1 Protocol "
			       "%02d\n", __func__, srds_s1);
		} else {
			do_phy_config(phy_config);
		}
		phy_config = get_phy_config(srds_s2,
					    serdes2_phy_config,
					    ARRAY_SIZE(serdes2_phy_config));
		if (!phy_config) {
			printf("%s qds: WRIOP: Unsupported SerDes2 Protocol "
			       "%02d\n", __func__, srds_s2);
		} else {
			do_phy_config(phy_config);
		}
		phy_config = get_phy_config(srds_s3,
					    serdes3_phy_config,
					    ARRAY_SIZE(serdes3_phy_config));
		if (!phy_config) {
			printf("%s qds: WRIOP: Unsupported SerDes3 Protocol "
			       "%02d\n", __func__, srds_s3);
		} else {
			do_phy_config(phy_config);
		}
	}

	wriop_set_phy_address(WRIOP1_DPMAC17, RGMII_PHY_ADDR1);
	wriop_set_phy_address(WRIOP1_DPMAC18, RGMII_PHY_ADDR2);

	/*Set MDIO bus 1 muxing front-ends for RGMII interfaces*/
	for (ioslot = EMI1_RGMII1; ioslot <= EMI1_RGMII2; ioslot++) {
		i = (ioslot - EMI1_RGMII1) + 1;
		bus = lx2160a_qds_mdio_init(EMI1, ioslot);
		if (!bus)
			printf("could not get bus for RGMII%d\n", i);
		else
			wriop_set_mdio(WRIOP1_DPMAC16 + i, bus);
	}

	cpu_eth_init(bis);
#endif /* CONFIG_FMAN_ENET */

#ifdef CONFIG_PHY_AQUANTIA
	/*
	 * Export functions to be used by AQ firmware
	 * upload application
	 */
	gd->jt->strcpy = strcpy;
	gd->jt->mdelay = mdelay;
	gd->jt->mdio_get_current_dev = mdio_get_current_dev;
	gd->jt->phy_find_by_mask = phy_find_by_mask;
	gd->jt->mdio_phydev_for_ethname = mdio_phydev_for_ethname;
	gd->jt->miiphy_set_current_dev = miiphy_set_current_dev;
#endif
#endif
	return pci_eth_init(bis);
}

#if defined(CONFIG_RESET_PHY_R)
void reset_phy(void)
{
#if defined(CONFIG_FSL_MC_ENET)
	mc_env_boot();
#endif
}
#endif /* CONFIG_RESET_PHY_R */
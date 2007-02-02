/** @file
 *
 * VBox network devices:
 * NAT network transport driver
 */

/*
 * Copyright (C) 2006 InnoTek Systemberatung GmbH
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * in version 2 as it comes in the "COPYING" file of the VirtualBox OSE
 * distribution. VirtualBox OSE is distributed in the hope that it will
 * be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * If you received this file as part of a commercial VirtualBox
 * distribution, then only the terms of your commercial VirtualBox
 * license agreement apply instead of the previous paragraph.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_DRV_NAT
#include "Network/slirp/libslirp.h"
#include <VBox/pdm.h>
#include <VBox/cfgm.h>
#include <VBox/mm.h>
#include <VBox/err.h>

#include <VBox/log.h>
#include <iprt/assert.h>
#include <iprt/file.h>
#include <iprt/string.h>

#include "Builtins.h"


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
/**
 * Block driver instance data.
 */
typedef struct DRVNAT
{
    /** The network interface. */
    PDMINETWORKCONNECTOR    INetworkConnector;
    /** The port we're attached to. */
    PPDMINETWORKPORT        pPort;
    /** Pointer to the driver instance. */
    PPDMDRVINS              pDrvIns;

} DRVNAT, *PDRVNAT;

/** Converts a pointer to NAT::INetworkConnector to a PRDVNAT. */
#define PDMINETWORKCONNECTOR_2_DRVNAT(pInterface)   ( (PDRVNAT)((uintptr_t)pInterface - RT_OFFSETOF(DRVNAT, INetworkConnector)) )


/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/** @todo change this into a MAC -> PDRVNAT translation list. */
/** Pointer to the driver instance.
 * This is required by slirp. */
static PDRVNAT          g_pDrv = NULL;
#if 0
/** If set the thread should terminate. */
static bool             g_fThreadTerm = false;
/** The thread id of the select thread (drvNATSelectThread()). */
static RTTHREAD         g_ThreadSelect;

static RTCRITSECT       g_CritSectSlirp;
#endif


/**
 * Send data to the network.
 *
 * @returns VBox status code.
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @param   pvBuf           Data to send.
 * @param   cb              Number of bytes to send.
 * @thread  EMT
 */
static DECLCALLBACK(int) drvNATSend(PPDMINETWORKCONNECTOR pInterface, const void *pvBuf, size_t cb)
{
    LogFlow(("drvNATSend: pvBuf=%p cb=%#x\n", pvBuf, cb));
    Log2(("drvNATSend: pvBuf=%p cb=%#x\n"
          "%.*Vhxd\n",
          pvBuf, cb, cb, pvBuf));
    slirp_input((uint8_t *)pvBuf, cb);
    return VINF_SUCCESS;
}


/**
 * Send multiple data packets to the network.
 *
 * @returns VBox status code.
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @param   cPackets        Number of packets
 * @param   paPacket        Packet description array
 * @thread  EMT
 */
static DECLCALLBACK(int) drvNATSendEx(PPDMINETWORKCONNECTOR pInterface, uint32_t cPackets, PPDMINETWORKPACKET paPacket)
{
    int rc = VERR_INVALID_PARAMETER;

    for (uint32_t i=0;i<cPackets;i++)
    {
        rc = drvNATSend(pInterface, paPacket[i].pvBuf, paPacket[i].cb);
        if (VBOX_FAILURE(rc))
            break;
    }
    return rc;
}

/**
 * Set promiscuous mode.
 *
 * This is called when the promiscuous mode is set. This means that there doesn't have
 * to be a mode change when it's called.
 *
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @param   fPromiscuous    Set if the adaptor is now in promiscuous mode. Clear if it is not.
 * @thread  EMT
 */
static DECLCALLBACK(void) drvNATSetPromiscuousMode(PPDMINETWORKCONNECTOR pInterface, bool fPromiscuous)
{
    LogFlow(("drvNATSetPromiscuousMode: fPromiscuous=%d\n", fPromiscuous));
    /* nothing to do */
}


/**
 * Notification on link status changes.
 *
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @param   enmLinkState    The new link state.
 * @thread  EMT
 */
static DECLCALLBACK(void) drvNATNotifyLinkChanged(PPDMINETWORKCONNECTOR pInterface, PDMNETWORKLINKSTATE enmLinkState)
{
    LogFlow(("drvNATNotifyLinkChanged: enmLinkState=%d\n", enmLinkState));
    switch (enmLinkState)
    {
        case PDMNETWORKLINKSTATE_UP:
            LogRel(("NAT: link up\n"));
            slirp_link_up();
            break;
        case PDMNETWORKLINKSTATE_DOWN:
        case PDMNETWORKLINKSTATE_DOWN_RESUME:
            LogRel(("NAT: link down\n"));
            slirp_link_down();
            break;
        default:
            AssertMsgFailed(("drvNATNotifyLinkChanged: unexpected link state %d\n", enmLinkState));
    }
}


/**
 * More receive buffer has become available.
 *
 * This is called when the NIC frees up receive buffers.
 *
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @thread  EMT
 */
static DECLCALLBACK(void) drvNATNotifyCanReceive(PPDMINETWORKCONNECTOR pInterface)
{
    LogFlow(("drvNATNotifyCanReceive:\n"));
    /** @todo do something useful here. */
}


/**
 * Poller callback.
 */
static DECLCALLBACK(void) drvNATPoller(PPDMDRVINS pDrvIns)
{
    fd_set  ReadFDs;
    fd_set  WriteFDs;
    fd_set  XcptFDs;
    int     cFDs = -1;
    FD_ZERO(&ReadFDs);
    FD_ZERO(&WriteFDs);
    FD_ZERO(&XcptFDs);
    slirp_select_fill(&cFDs, &ReadFDs, &WriteFDs, &XcptFDs);

    struct timeval tv = {0, 0}; /* no wait */
    int cReadFDs = select(cFDs + 1, &ReadFDs, &WriteFDs, &XcptFDs, &tv);
    if (cReadFDs >= 0)
        slirp_select_poll(&ReadFDs, &WriteFDs, &XcptFDs);
}


/**
 * Function called by slirp to check if it's possible to feed incoming data to the network port.
 * @returns 1 if possible.
 * @returns 0 if not possible.
 */
int slirp_can_output(void)
{
    if (g_pDrv)
        return g_pDrv->pPort->pfnCanReceive(g_pDrv->pPort);
    return 0;
}


/**
 * Function called by slirp to feed incoming data to the network port.
 */
void slirp_output(const uint8_t *pu8Buf, int cb)
{
    Log2(("slirp_output: pu8Buf=%p cb=%#x (g_pDrv=%p)\n"
          "%.*Vhxd\n",
          pu8Buf, cb, g_pDrv,
          cb, pu8Buf));
    if (g_pDrv)
    {
        int rc = g_pDrv->pPort->pfnReceive(g_pDrv->pPort, pu8Buf, cb);
        AssertRC(rc); NOREF(rc);
    }
}

/**
 * Queries an interface to the driver.
 *
 * @returns Pointer to interface.
 * @returns NULL if the interface was not supported by the driver.
 * @param   pInterface          Pointer to this interface structure.
 * @param   enmInterface        The requested interface identification.
 * @thread  Any thread.
 */
static DECLCALLBACK(void *) drvNATQueryInterface(PPDMIBASE pInterface, PDMINTERFACE enmInterface)
{
    PPDMDRVINS pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVNAT pData = PDMINS2DATA(pDrvIns, PDRVNAT);
    switch (enmInterface)
    {
        case PDMINTERFACE_BASE:
            return &pDrvIns->IBase;
        case PDMINTERFACE_NETWORK_CONNECTOR:
            return &pData->INetworkConnector;
        default:
            return NULL;
    }
}


/**
 * Destruct a driver instance.
 *
 * Most VM resources are freed by the VM. This callback is provided so that any non-VM
 * resources can be freed correctly.
 *
 * @param   pDrvIns     The driver instance data.
 */
static DECLCALLBACK(void) drvNATDestruct(PPDMDRVINS pDrvIns)
{
    LogFlow(("drvNATDestruct:\n"));
#if ARCH_BITS == 64
    LogRel(("NAT: g_cpvHashUsed=%RU32 g_cpvHashCollisions=%RU32 g_cpvHashInserts=%RU64 g_cpvHashDone=%RU64\n",
            g_cpvHashUsed, g_cpvHashCollisions, g_cpvHashInserts, g_cpvHashDone));
#endif 
    slirp_term();
    g_pDrv = NULL;
}


/**
 * Sets up the redirectors.
 *
 * @returns VBox status code.
 * @param   pCfgHandle      The drivers configuration handle.
 */
static int drvNATConstructRedir(PCFGMNODE pCfgHandle)
{
    /*
     * Enumerate redirections.
     */
    for (PCFGMNODE pNode = CFGMR3GetFirstChild(pCfgHandle); pNode; pNode = CFGMR3GetNextChild(pNode))
    {
        /* protocol type */
        bool fUDP;
        int rc = CFGMR3QueryBool(pNode, "UDP", &fUDP);
        if (rc == VERR_CFGM_VALUE_NOT_FOUND)
            fUDP = true;
        else if (VBOX_FAILURE(rc))
        {
            AssertMsgFailed(("Configuration error: Boolean \"UDP\" -> %Vrc\n", rc));
            return rc;
        }

        /* host port */
        int32_t iHostPort;
        rc = CFGMR3QueryS32(pNode, "HostPort", &iHostPort);
        if (VBOX_FAILURE(rc))
        {
            AssertMsgFailed(("Configuration error: Boolean \"HostPort\" -> %Vrc\n", rc));
            return rc;
        }

        /* guest port */
        int32_t iGuestPort;
        rc = CFGMR3QueryS32(pNode, "GuestPort", &iGuestPort);
        if (VBOX_FAILURE(rc))
        {
            AssertMsgFailed(("Configuration error: Boolean \"GuestPort\" -> %Vrc\n", rc));
            return rc;
        }

        /* guest address */
        char    szGuestIP[32];
        rc = CFGMR3QueryString(pNode, "GuestIP", &szGuestIP[0], sizeof(szGuestIP));
        if (rc == VERR_CFGM_VALUE_NOT_FOUND)
            strcpy(szGuestIP, "10.0.2.15");
        else if (VBOX_FAILURE(rc))
        {
            AssertMsgFailed(("Configuration error: Boolean \"HostPort\" -> %Vrc\n", rc));
            return rc;
        }
        struct in_addr GuestIP;
        if (!inet_aton(szGuestIP, &GuestIP))
        {
            AssertMsgFailed(("Configuration error: Invalid \"GuestIP\"=\"%s\", inet_aton failed.\n", szGuestIP));
            return VERR_NAT_REDIR_GUEST_IP;
        }

        /*
         * Call slirp about it.
         */
        Log(("drvNATConstruct: Redir %d -> %s:%d\n", iHostPort, szGuestIP, iGuestPort));
        if (slirp_redir(fUDP, iHostPort, GuestIP, iGuestPort) < 0)
        {
            AssertMsgFailed(("Configuration error: failed to setup redirection of %d to %s:%d. Probably a conflict with existing services or other rules.\n",
                             iHostPort, szGuestIP, iGuestPort));
            return VERR_NAT_REDIR_SETUP;
        }
    } /* for each redir rule */

    return VINF_SUCCESS;
}


/**
 * Construct a NAT network transport driver instance.
 *
 * @returns VBox status.
 * @param   pDrvIns     The driver instance data.
 *                      If the registration structure is needed, pDrvIns->pDrvReg points to it.
 * @param   pCfgHandle  Configuration node handle for the driver. Use this to obtain the configuration
 *                      of the driver instance. It's also found in pDrvIns->pCfgHandle, but like
 *                      iInstance it's expected to be used a bit in this function.
 */
static DECLCALLBACK(int) drvNATConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfgHandle)
{
    PDRVNAT pData = PDMINS2DATA(pDrvIns, PDRVNAT);
    LogFlow(("drvNATConstruct:\n"));

    /*
     * Validate the config.
     */
    if (!CFGMR3AreValuesValid(pCfgHandle, "\0"))
        return PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRVINS_UNKNOWN_CFG_VALUES, "");

    /*
     * Init the static parts.
     */
    pData->pDrvIns                      = pDrvIns;
    /* IBase */
    pDrvIns->IBase.pfnQueryInterface    = drvNATQueryInterface;
    /* INetwork */
    pData->INetworkConnector.pfnSend               = drvNATSend;
    pData->INetworkConnector.pfnSendEx             = drvNATSendEx;
    pData->INetworkConnector.pfnSetPromiscuousMode = drvNATSetPromiscuousMode;
    pData->INetworkConnector.pfnNotifyLinkChanged  = drvNATNotifyLinkChanged;
    pData->INetworkConnector.pfnNotifyCanReceive   = drvNATNotifyCanReceive;

    /*
     * Query the network port interface.
     */
    pData->pPort = (PPDMINETWORKPORT)pDrvIns->pUpBase->pfnQueryInterface(pDrvIns->pUpBase, PDMINTERFACE_NETWORK_PORT);
    if (!pData->pPort)
        return PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_MISSING_INTERFACE_ABOVE,
                                N_("Configuration error: the above device/driver didn't export the network port interface!\n"));

#if 0
    /*
     * The slirp lock and thread sem.
     */
    int rc = RTCritSectInit(&g_CritSectSlirp);
    if (VBOX_FAILURE(rc))
        return rc;
    rc = RTSemEventCreate(&g_EventSem);
    if (VBOX_SUCCESS(rc))
    {
        /*
         * Start the select thread. (it'll block on the sem)
         */
        g_fThreadTerm = false;
        rc = RTThreadCreate(&g_ThreadSelect, drvNATSelectThread, 0, NULL, "NATSEL");
        if (VBOX_SUCCESS(rc))
        {
#endif
            int rc;
            /*
             * Initialize slirp.
             */
            rc = slirp_init();
            if (VBOX_SUCCESS(rc))
            {
                rc = drvNATConstructRedir(pCfgHandle);
                if (VBOX_SUCCESS(rc))
                {
                    pDrvIns->pDrvHlp->pfnPDMPollerRegister(pDrvIns, drvNATPoller);
                    g_pDrv = pData;
#if 0
                    RTSemEventSignal(g_EventSem);
                    RTThreadSleep(0);
#endif
                    return VINF_SUCCESS;
                }
                /* failure path */
                slirp_term();
            }
            else
            {
                switch (rc)
                {
                    case VERR_NAT_DNS:
                        PDMDRV_SET_ERROR(pDrvIns, rc, N_("Domain Name Server (DNS) for NAT networking could not be determined"));
                        break;
                    default:
                        PDMDRV_SET_ERROR(pDrvIns, rc, N_("Unknown error during NAT networking setup: "));
                        AssertMsgFailed(("Add error message for rc=%d (%Vrc)\n", rc, rc));
                        break;
                }
            }
#if 0
            g_fThreadTerm = true;
            RTSemEventSignal(g_EventSem);
            RTThreadSleep(0);
        }
        RTSemEventDestroy(g_EventSem);
        g_EventSem = NULL;
    }
    RTCritSectDelete(&g_CritSectSlirp);
#endif
    return rc;
}



/**
 * NAT network transport driver registration record.
 */
const PDMDRVREG g_DrvNAT =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szDriverName */
    "NAT",
    /* pszDescription */
    "NAT Network Transport Driver",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_NETWORK,
    /* cMaxInstances */
    1,
    /* cbInstance */
    sizeof(DRVNAT),
    /* pfnConstruct */
    drvNATConstruct,
    /* pfnDestruct */
    drvNATDestruct,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL
};

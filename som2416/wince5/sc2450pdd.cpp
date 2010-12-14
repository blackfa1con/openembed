/**
THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
PARTICULAR PURPOSE.
Copyright (c) 2006 Samsung Electronics, co. ltd  All rights reserved.

Module Name:  

    SC2450PDD.CPP	

Abstract:

    S3C2450 USB2.0 function device driver 

rev:

    2006.9.4	: First release (Woosuk Chung)
 
**/
#include <windows.h>
#include <nkintr.h>
#include <ddkreg.h>
#include <S3C2450.h>				
#include "sc2450pdd.h"
#if (BSP_TYPE == BSP_SMDK2443)
#include <S3C2450REF_GPIO.h> //this is renamed from #include <S3C2443REF_GPIO.h>, Same File
#elif (BSP_TYPE == BSP_SMDK2450)
#endif
#include <bsp.h>

//[david.modify] 2008-06-17 12:12
// fixbug:
//Also, I checked your USB source code(sc2450pdd.cpp), But GPIO for USB power controll is not modified. 
//It still use GPH14. 

#include <xllp_gpio_david.h>
#include <dbgmsg_david.h>
#define S805G_USBPWR 1		//使用GPB4而不是GPH14
stGPIOInfo g_stGPIOInfo[]={
	{ PWREN_USB,  1, ALT_FUNC_OUT, PULL_UP_ENABLE},		
	};

static volatile BSP_ARGS *v_gBspArgs;
#define CARD_INSERTED 1
#define CARD_REMOVED 2
#define UDC_REG_PRIORITY_VAL _T("Priority256")
#define DBG 0

#define TEST_MODE_SUPPORT 1

#define INVALID_HANDLE         (HANDLE)-1

#if TEST_MODE_SUPPORT
#define USB_TEST_J 				0x01
#define USB_TEST_K 				0x02
#define USB_TEST_SE0_NAK 		0x03
#define USB_TEST_PACKET 		0x04
#define USB_TEST_FORCE_ENABLE 0x05

#define USB_FEATURE_TEST_MODE 2


#define TEST_PKT_SIZE 53
#define TEST_ARR_SIZE 27

WORD ahwTestPkt [TEST_ARR_SIZE] = {

	0x0000, 0x0000, 0x0000, 
	0xAA00, 0xAAAA, 0xAAAA, 0xAAAA,
	0xEEAA, 0xEEEE, 0xEEEE, 0xEEEE,
	0xFEEE,	0xFFFF, 0xFFFF, 0xFFFF,
	0xFFFF, 0xFFFF, 0x7FFF, 0xDFBF,
	0xF7EF, 0xFDFB, 0x7EFC, 0xDFBF,
	0xF7EF, 0xFDFB, 0x007E, 0x0000 

};

#endif

enum EP0_STATE {
    EP0_STATE_IDLE = 0,
    EP0_STATE_IN_DATA_PHASE,
    EP0_STATE_OUT_DATA_PHASE
};

#define IN_TRANSFER  1
#define OUT_TRANSFER 2

typedef struct EP_STATUS {
    DWORD                   dwEndpointNumber;
    DWORD                   dwDirectionAssigned;
    DWORD                   dwPacketSizeAssigned;
    BOOL                    fInitialized;
    DWORD                   dwEndpointType;
    PSTransfer              pTransfer;
    CRITICAL_SECTION        cs;
} *PEP_STATUS;

#define LOCK_ENDPOINT(peps)     EnterCriticalSection(&peps->cs)
#define UNLOCK_ENDPOINT(peps)   LeaveCriticalSection(&peps->cs)


#define EP_0_PACKET_SIZE    0x40    // usb2.0  

#define ENDPOINT_COUNT  9
#define EP_VALID(x)     ((x) < ENDPOINT_COUNT)


#define DEFAULT_PRIORITY 100


typedef struct CTRL_PDD_CONTEXT {
    PVOID             pvMddContext;
    DWORD             dwSig;
    HANDLE            hIST;
    HANDLE            hevInterrupt;
    BOOL              fRunning;
    CRITICAL_SECTION  csIndexedRegisterAccess;
    BOOL              fSpeedReported;
    BOOL              fRestartIST;
    BOOL              fExitIST;
    BOOL              attachedState;
    BOOL              sendDataEnd;
    EP0_STATE         Ep0State;
    
    // registry 
    DWORD             dwIOBase;
    DWORD             dwSysIntr;
    DWORD             dwIrq;
    DWORD             dwIOLen;
    DWORD             dwISTPriority;

	DWORD			  dwResetCount;

    USB_DEVICE_REQUEST udr;
    EP_STATUS         rgEpStatus[ENDPOINT_COUNT];
    
    PFN_UFN_MDD_NOTIFY      pfnNotify;
    HANDLE                  hBusAccess;
    CEDEVICE_POWER_STATE    cpsCurrent;
} *PCTRLR_PDD_CONTEXT;

#define SC2450_SIG '2450' // "SC2450" signature

#define IS_VALID_SC2450_CONTEXT(ptr) \
    ( (ptr != NULL) && (ptr->dwSig == SC2450_SIG) )


#ifdef DEBUG

#define ZONE_POWER           DEBUGZONE(8)

UFN_GENERATE_DPCURSETTINGS(UFN_DEFAULT_DPCURSETTINGS_NAME, 
    _T("Power"), _T(""), _T(""), _T(""), 
    DBG_ERROR | DBG_INIT); 
// Caution: Turning on more debug zones can cause STALLs due
// to corrupted setup packets.

// Validate the context.
static
VOID
ValidateContext(
                PCTRLR_PDD_CONTEXT pContext
                )
{
    PREFAST_DEBUGCHK(pContext);
    DEBUGCHK(pContext->dwSig == SC2450_SIG);
    DEBUGCHK(!pContext->hevInterrupt || pContext->hIST);
    DEBUGCHK(VALID_DX(pContext->cpsCurrent));
    DEBUGCHK(pContext->pfnNotify);
}

#else
#define ValidateContext(ptr)
#endif

volatile BYTE *g_pUDCBase;

static BYTE USBClassInfo;
#define USB_RNDIS  0
#define USB_Serial 1
#define USB_MSF    2

#define SET   TRUE
#define CLEAR FALSE

#define CTRLR_BASE_REG_ADDR(offset) ((volatile ULONG*) ( (g_pUDCBase) + (offset)))

#define DRIVER_USB_KEY TEXT("Drivers\\USB\\FunctionDrivers")
#define DRIVER_USB_VALUE TEXT("DefaultClientDriver")

#define CLKCON_USBD (1<<7)

volatile S3C2450_CLKPWR_REG *pCLKPWR	= NULL;		// Clock power registers (needed to enable I2S and SPI clocks)
volatile S3C2450_IOPORT_REG *pIOPregs	= NULL;

// EINT2 Test Code by woo
UINT32 g_PlugIrq = IRQ_EINT2;	// Determined by SMDK2450 board layout.
UINT32 g_PlugSysIntr = SYSINTR_UNDEFINED;
HANDLE g_PlugThread;
HANDLE g_PlugEvent;
HANDLE g_SdCardDetectThread;

HANDLE g_tmpThread;
static DWORD PLUG_IST();
static DWORD SDCARD_DetectIST();

BOOL HW_USBClocks(CEDEVICE_POWER_STATE    cpsNew);

DWORD bSDMMCMSF = TRUE;
#define HIGH 1
#define FULL  0

#define POWER_THREAD_PRIORITY 101
// end woo
// Read a register.
inline
WORD
ReadReg(
        PCTRLR_PDD_CONTEXT pContext,
        DWORD dwOffset
        )
{
    DEBUGCHK(IS_VALID_SC2450_CONTEXT(pContext));

    volatile ULONG *pbReg = CTRLR_BASE_REG_ADDR(dwOffset);
    WORD bValue = (WORD) *pbReg;
    return bValue;
}


// Write a register.
inline
VOID
WriteReg(
         PCTRLR_PDD_CONTEXT pContext,
         DWORD dwOffset,
         DWORD bValue
         )
{
    DEBUGCHK(IS_VALID_SC2450_CONTEXT(pContext));

    volatile ULONG *pbReg = CTRLR_BASE_REG_ADDR(dwOffset);
    *pbReg = (ULONG) bValue;
}



// Calling with dwMask = 0 and SET reads and writes the contents unchanged.
inline
BYTE
SetClearReg(
            PCTRLR_PDD_CONTEXT pContext,
            DWORD dwOffset,
            WORD dwMask,
            BOOL  bSet
            )
{
    DEBUGCHK(IS_VALID_SC2450_CONTEXT(pContext));

    volatile ULONG *pbReg = CTRLR_BASE_REG_ADDR(dwOffset);
    BYTE bValue = (BYTE) *pbReg;

    if (bSet) {
        bValue |= dwMask;
    }
    else {
        bValue &= ~dwMask;
    }

    *pbReg = bValue;

    return bValue;
}
// Calling with dwMask = 0 and SET reads and writes the contents unchanged.
inline
BYTE
SetClearIndexedReg(
                   PCTRLR_PDD_CONTEXT pContext,
                   DWORD dwEndpoint,
                   DWORD dwOffset,
                   WORD dwMask,
                   BOOL  bSet
                   )
{
    DEBUGCHK(IS_VALID_SC2450_CONTEXT(pContext));
    BYTE bValue = 0;

    EnterCriticalSection(&pContext->csIndexedRegisterAccess);
    // Write the EP number to the index reg
    WriteReg(pContext, IR, (BYTE) dwEndpoint);    
    // Now Write the Register associated with this Endpoint for a given offset
    bValue = SetClearReg(pContext, dwOffset, dwMask, bSet);
    LeaveCriticalSection(&pContext->csIndexedRegisterAccess);

    return bValue;
}

// Read an indexed register.
inline
WORD
ReadIndexedReg(
               PCTRLR_PDD_CONTEXT pContext,
               DWORD dwEndpoint,
               DWORD regOffset
               )
{
    DEBUGCHK(IS_VALID_SC2450_CONTEXT(pContext));

    EnterCriticalSection(&pContext->csIndexedRegisterAccess);
    // Write the EP number to the index reg
    WriteReg(pContext, IR, (BYTE) dwEndpoint);
    // Now Read the Register associated with this Endpoint for a given offset
    WORD bValue = ReadReg(pContext, regOffset);
    LeaveCriticalSection(&pContext->csIndexedRegisterAccess);
    return bValue;
}


// Write an indexed register.
inline
VOID
WriteIndexedReg(
                PCTRLR_PDD_CONTEXT pContext,
                DWORD dwEndpoint,
                DWORD regOffset,
                DWORD  bValue
                )
{  
    DEBUGCHK(IS_VALID_SC2450_CONTEXT(pContext));

    EnterCriticalSection(&pContext->csIndexedRegisterAccess);
    // Write the EP number to the index reg
    WriteReg(pContext, IR, (BYTE) dwEndpoint);    
    // Now Write the Register associated with this Endpoint for a given offset
    WriteReg(pContext, regOffset, bValue);
    LeaveCriticalSection(&pContext->csIndexedRegisterAccess);
}

/*++

Routine Description:

Return the data register of an endpoint.

Arguments:

dwEndpoint - the target endpoint

Return Value:

The data register of the target endpoint.

--*/
static
volatile ULONG*
_GetDataRegister(
                 DWORD        dwEndpoint    
                 )
{
    volatile ULONG *pulDataReg = NULL;

    //
    // find the data register (non-uniform offset)
    //
    switch (dwEndpoint) {
        case  0: pulDataReg = CTRLR_BASE_REG_ADDR(EP0BR);  break;
        case  1: pulDataReg = CTRLR_BASE_REG_ADDR(EP1BR);  break;
        case  2: pulDataReg = CTRLR_BASE_REG_ADDR(EP2BR);  break;
        case  3: pulDataReg = CTRLR_BASE_REG_ADDR(EP3BR);  break;
        case  4: pulDataReg = CTRLR_BASE_REG_ADDR(EP4BR);  break;
        case  5: pulDataReg = CTRLR_BASE_REG_ADDR(EP5BR);  break;
        case  6: pulDataReg = CTRLR_BASE_REG_ADDR(EP6BR);  break;
        case  7: pulDataReg = CTRLR_BASE_REG_ADDR(EP7BR);  break;
        case  8: pulDataReg = CTRLR_BASE_REG_ADDR(EP8BR);  break;
        default:
            DEBUGCHK(FALSE);
            break;
    }

    return pulDataReg;
} // _GetDataRegister


// Retrieve the endpoint status structure.
inline
static
PEP_STATUS
GetEpStatus(
            PCTRLR_PDD_CONTEXT pContext,
            DWORD dwEndpoint
            )
{
    ValidateContext(pContext);
    DEBUGCHK(EP_VALID(dwEndpoint));

    PEP_STATUS peps = &pContext->rgEpStatus[dwEndpoint];

    return peps;
}


// Return the irq bit for this endpoint.
inline
static
BYTE
EpToIrqStatBit(
               DWORD dwEndpoint
               )
{
    DEBUGCHK(EP_VALID(dwEndpoint));

    return (1 << (BYTE)dwEndpoint);
}

/*++

Routine Description:

Enable the interrupt of an endpoint.

Arguments:

dwEndpoint - the target endpoint

Return Value:

None.

--*/
static
VOID
EnableDisableEndpointInterrupt(
                        PCTRLR_PDD_CONTEXT  pContext,
                        DWORD               dwEndpoint,
                        BOOL                fEnable
                        )
{
    // TODO: Make new cs or rename this one
    EnterCriticalSection(&pContext->csIndexedRegisterAccess);
    
    // Disable the Endpoint Interrupt
    WORD bEpIntReg = ReadReg(pContext, EIER);
    BYTE bIrqEnBit = EpToIrqStatBit(dwEndpoint);

    if (fEnable) {
        bEpIntReg |= bIrqEnBit;
    }
    else {
        bEpIntReg &= ~bIrqEnBit;
    }
    
    WriteReg(pContext, EIER, bEpIntReg);

    LeaveCriticalSection(&pContext->csIndexedRegisterAccess);    
}

static
inline
VOID
EnableEndpointInterrupt(
    PCTRLR_PDD_CONTEXT  pContext,
    DWORD               dwEndpoint
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();
    
    EnableDisableEndpointInterrupt(pContext, dwEndpoint, TRUE);

    FUNCTION_LEAVE_MSG();
}

static
inline
VOID
DisableEndpointInterrupt(
    PCTRLR_PDD_CONTEXT  pContext,
    DWORD               dwEndpoint
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();
    
    EnableDisableEndpointInterrupt(pContext, dwEndpoint, FALSE);

    FUNCTION_LEAVE_MSG();
}

/*++

Routine Description:

Clear the interrupt status register index of an endpoint.

Arguments:

dwEndpoint - the target endpoint

Return Value:

None.

--*/
static
VOID
ClearEndpointInterrupt(
                       PCTRLR_PDD_CONTEXT pContext,
                       DWORD        dwEndpoint
                       )
{    
    SETFNAME();
    FUNCTION_ENTER_MSG();
    
    // Clear the Endpoint Interrupt
    BYTE bIntBit = EpToIrqStatBit(dwEndpoint);
    WriteReg(pContext, EIR, bIntBit);

    FUNCTION_LEAVE_MSG();
} // _ClearInterrupt


// Reset an endpoint

VOID
ResetEndpoint(
              PCTRLR_PDD_CONTEXT pContext,
              EP_STATUS *peps
              )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    ValidateContext(pContext);
    PREFAST_DEBUGCHK(peps);

   RETAILMSG(DBG, (_T(" ResetEndpoint \r\n")));

    // Since Reset can be called before/after an Endpoint has been configured,
    // it is best to clear all IN and OUT bits associated with endpoint. 
    DWORD dwEndpoint = peps->dwEndpointNumber;
	//0816
	WORD bEDR;
    if(dwEndpoint == 0 ) {
        // Clear all EP0 Status bits
        WriteReg(pContext, EP0SR, (EP0SHT | EP0TST |EP0RSR));
        WriteReg(pContext, EP0CR, 0x0000);
    }
    else if(dwEndpoint < ENDPOINT_COUNT) {
        // Clear all EP Status bits
        WriteIndexedReg(pContext, dwEndpoint, ESR, (OSD | DTCZ | SPT | FFS | FSC | TPS));
        // Clear all EP Control bits
        bEDR = ReadIndexedReg(pContext, dwEndpoint, EDR);
        if(bEDR & (0x1 << dwEndpoint)) {		// Tx
             WriteIndexedReg(pContext, dwEndpoint, ECR, 0x0000);  // Single Buffer Mode
        	}
        else
        	{
             WriteIndexedReg(pContext, dwEndpoint, ECR, 0x0080); // Dual Buffer Mode
        	}
        

        // Clear and disable endpoint interrupt
        WriteReg(pContext, EIR, EpToIrqStatBit(peps->dwEndpointNumber));
        DisableEndpointInterrupt(pContext, peps->dwEndpointNumber);
    }
    else {
        DEBUGCHK(FALSE);
    }

    FUNCTION_LEAVE_MSG();
}



// Reset the device and EP0.

VOID
ResetDevice(
            PCTRLR_PDD_CONTEXT pContext
            )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    DEBUGCHK(IS_VALID_SC2450_CONTEXT(pContext));

    RETAILMSG(DBG, (_T(" ResetDevice \r\n")));

    // Reset System Control Register
    WriteReg(pContext, SCR, 0x0020);

    // Reset System Status Register
    WriteReg(pContext, SSR, (VBUSOFF | VBUSON | TBM | SDE | HFRM | HFSUSP | HFRES ) );


    // Disable endpoint interrupts - write Zeros to Disable
    WriteReg(pContext, EIER, EP_INTERRUPT_DISABLE_ALL);

    // End point Interrupt Status - Write a '1' to Clear
    WriteReg(pContext, EIR, CLEAR_ALL_EP_INTRS);

    // Reset all endpoints
    for (DWORD dwEpIdx = 0; dwEpIdx < ENDPOINT_COUNT; ++dwEpIdx) {
        EP_STATUS *peps = GetEpStatus(pContext, dwEpIdx);
        ResetEndpoint(pContext, peps);
    }

    FUNCTION_LEAVE_MSG();
}


static
VOID
CompleteTransfer(
                 PCTRLR_PDD_CONTEXT pContext,
                 PEP_STATUS peps,
                 DWORD dwUsbError
                 )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    PSTransfer pTransfer = peps->pTransfer;
    peps->pTransfer = NULL;

    pTransfer->dwUsbError = dwUsbError;
    pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_TRANSFER_COMPLETE, 
        (DWORD) pTransfer);

    FUNCTION_LEAVE_MSG();
}


#ifdef DEBUG
static
VOID
ValidateTransferDirection(
                          PCTRLR_PDD_CONTEXT pContext,
                          PEP_STATUS peps,
                          PSTransfer pTransfer
                          )
{
    DEBUGCHK(pContext);
    PREFAST_DEBUGCHK(peps);
    PREFAST_DEBUGCHK(pTransfer);

    if (peps->dwEndpointNumber != 0) {
        DEBUGCHK(peps->dwDirectionAssigned == pTransfer->dwFlags);
    }
}
#else
#define ValidateTransferDirection(ptr1, ptr2, ptr3)
#endif


// Read data from an endpoint.

static 
VOID
HandleRx(
         PCTRLR_PDD_CONTEXT       pContext,
         PEP_STATUS peps,
         PBOOL pfCompleted,
         PDWORD pdwStatus
         )
{

    BOOL fCompleted = FALSE;
    DWORD dwStatus = ERROR_GEN_FAILURE;
    DWORD dwEndpoint = peps->dwEndpointNumber;
    BYTE bRet = 0;
    WORD bEpIrqStat;
	
    SETFNAME();
    FUNCTION_ENTER_MSG();

    bEpIrqStat = ReadIndexedReg(pContext, dwEndpoint, ESR);

    PSTransfer pTransfer = peps->pTransfer;

    pTransfer = peps->pTransfer;
	if (pTransfer)
	{
        DEBUGCHK(pTransfer->dwFlags == USB_OUT_TRANSFER);
        DEBUGCHK(pTransfer->dwUsbError == UFN_NOT_COMPLETE_ERROR);

        ValidateTransferDirection(pContext, peps, pTransfer);

        DEBUGCHK(peps->fInitialized);

        DWORD dwCurrentPermissions = GetCurrentPermissions();
        SetProcPermissions(pTransfer->dwCallerPermissions);

		__try
		{
            volatile ULONG *pulFifoReg = _GetDataRegister(dwEndpoint);
            DEBUGCHK(pulFifoReg != NULL);

            PBYTE  pbBuffer =  (PBYTE)pTransfer->pvBuffer + pTransfer->cbTransferred;

            DWORD cbBuffer = pTransfer->cbBuffer - pTransfer->cbTransferred;
            DWORD cbFifoWord = ReadIndexedReg(pContext, dwEndpoint, BRCR);
			
	     WORD ReadData = 0 ;
	     DWORD cbFifo;

			if(dwEndpoint ==0)
			{
		     if (bEpIrqStat & EP0LWO) 
			 	cbFifo = cbFifoWord*2-1;
		     else 
			 	cbFifo = cbFifoWord*2;
	   	}
			else
			{
			if (bEpIrqStat & LWO) 
			 	cbFifo = cbFifoWord*2-1;
		     else 
			 	cbFifo = cbFifoWord*2;
	    	}
			
            DEBUGCHK(cbFifo <= peps->dwPacketSizeAssigned);

            // Read from the FIFO
            const DWORD cbRead = min(cbFifo, cbBuffer);
            DWORD cbToRead;
			for (cbToRead = 0; cbToRead < cbRead; cbToRead+=2)
			{
		      ReadData= (WORD)*pulFifoReg;
		      *pbBuffer = (BYTE)ReadData;
		      *(pbBuffer+1) = (BYTE)(ReadData>>8);
		      pbBuffer +=2;
                }

            pTransfer->cbTransferred += cbRead;

			if ( (cbRead < peps->dwPacketSizeAssigned) || (pTransfer->cbTransferred == pTransfer->cbBuffer) )
			{
                // Short packet or filled buffer. Complete transfer.
                fCompleted = TRUE;
                dwStatus = UFN_NO_ERROR;
            }

			if (dwEndpoint == 0)
			{
		 	SetClearReg(pContext, EP0SR, EP0RSR, SET); 
				if (fCompleted)
				{
                    pContext->Ep0State = EP0_STATE_IDLE;
                }
            }
			else
			{
				if((peps->dwEndpointType == USB_ENDPOINT_TYPE_BULK) && (!fCompleted))
				{
					if(bEpIrqStat & PSIF)
					{
				pbBuffer =  (PBYTE)pTransfer->pvBuffer + pTransfer->cbTransferred;
           			cbBuffer = pTransfer->cbBuffer - pTransfer->cbTransferred;
            			cbFifoWord = ReadIndexedReg(pContext, dwEndpoint, BRCR);
	      			ReadData = 0 ;

	     			if (bEpIrqStat & LWO) 
		 			cbFifo = cbFifoWord*2-1;
	     			else 
		 			cbFifo = cbFifoWord*2;

		            // Read from the FIFO
 			       const DWORD cbRead = min(cbFifo, cbBuffer);
						for (cbToRead = 0; cbToRead < cbRead; cbToRead+=2)
						{
		      			ReadData= (WORD)*pulFifoReg;
		      			*pbBuffer = (BYTE)ReadData;
		      			*(pbBuffer+1) = (BYTE)(ReadData>>8);
		      			pbBuffer +=2;
                		}

				pTransfer->cbTransferred += cbRead;

						if ( (cbRead < peps->dwPacketSizeAssigned) || (pTransfer->cbTransferred == pTransfer->cbBuffer) )
						{
   	  	              	// Short packet or filled buffer. Complete transfer.
   		              	fCompleted = TRUE;
        		       	dwStatus = UFN_NO_ERROR;
            			}
			}
					else
					{
				RETAILMSG(DBG, (_T("Now_NO One Packet, bEpIrqStat : 0x%08x \r\n"),bEpIrqStat));
				}
		}
            }
        }
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
            RETAILMSG(DBG, (_T("%s Exception!\r\n"), pszFname));
            fCompleted = TRUE;
            dwStatus = UFN_CLIENT_BUFFER_ERROR;
        }

        SetProcPermissions(dwCurrentPermissions);
    
		if (fCompleted)
		{
            RETAILMSG(DBG, (_T("%s RxDone Ep%x BufferSize=%u, Xfrd=%u\r\n"), 
                pszFname, dwEndpoint,pTransfer->cbBuffer, pTransfer->cbTransferred));
        }
    }
	else 
		SetClearIndexedReg(pContext,dwEndpoint,ESR,RPS,SET);
		
    *pfCompleted = fCompleted;
    *pdwStatus = dwStatus;
    FUNCTION_LEAVE_MSG();
}


// Write data to an endpoint.
static 
VOID
HandleTx(
         PCTRLR_PDD_CONTEXT       pContext,
         PEP_STATUS peps,
         BOOL fEnableInterrupts
         )
{
    SETFNAME();
    DEBUGCHK(pContext);
    PREFAST_DEBUGCHK(peps);

    // This routine can be entered from both ISTMain and MDD/Client threads so
    // need critical section.

    FUNCTION_ENTER_MSG();

#if 1
    BOOL fCompleted = FALSE; 
    PSTransfer pTransfer = peps->pTransfer;
    DWORD dwStatus = ERROR_GEN_FAILURE;
    DEBUGCHK(peps->fInitialized);
    DWORD dwEndpoint = peps->dwEndpointNumber;

    pTransfer = peps->pTransfer;

    if (pTransfer) {
        ValidateTransferDirection(pContext, peps, pTransfer);

        DEBUGCHK(pTransfer->dwFlags == USB_IN_TRANSFER);
        DEBUGCHK(pTransfer->dwUsbError == UFN_NOT_COMPLETE_ERROR);

        DWORD dwCurrentPermissions = GetCurrentPermissions();
        SetProcPermissions(pTransfer->dwCallerPermissions);

        // Transfer is ready
        __try {
            PBYTE pbBuffer = (PBYTE) pTransfer->pvBuffer + pTransfer->cbTransferred;
            DWORD cbBuffer = pTransfer->cbBuffer - pTransfer->cbTransferred;

			
            volatile ULONG *pulFifoReg = _GetDataRegister(dwEndpoint);

            DWORD cbWritten = 0;
	     WORD WriteData = 0;

            // Min of input byte count and supported size
            DWORD cbToWrite = min(cbBuffer, peps->dwPacketSizeAssigned); 

            if (dwEndpoint == 0) {

		  WriteIndexedReg(pContext, 0, BWCR, cbToWrite);
		  
                for (cbWritten = 0; cbWritten < cbToWrite; cbWritten+=2) {
		      WriteData=((*(pbBuffer+1))<<8) | *pbBuffer;	
                    *pulFifoReg = WriteData;
		      pbBuffer +=2;
                }

                /* We can complete on a packet which is full. We need to wait till
                * next time and generate a zero length packet, so only complete
                * if we're at the end and it is not the max packet size.
                */
                pTransfer->cbTransferred += cbToWrite;
				// We have to consider 16bit buffer interface
		  if (pTransfer->cbTransferred == pTransfer->cbBuffer 
					&& pTransfer->pvPddData == 0) {
				
                    dwStatus = UFN_NO_ERROR;
                    fCompleted = TRUE;
                    pContext->Ep0State = EP0_STATE_IDLE;
                }
                
            }
            else {
				
                // Enable Interrupts before writing to the FIFO. This insures
                // That any interrupts generated because of the write will be
                // "latched"
                if (fEnableInterrupts) {
                    DEBUGCHK(dwEndpoint != 0);
		       EnableEndpointInterrupt(pContext, dwEndpoint);
                }

		// If Packet was completed in a previous interrupt cycle,
		// the data length to be txed will be zero, and 
		// zero length data packet should not be transmitted 
		if( cbToWrite != 0) WriteIndexedReg(pContext, dwEndpoint, BWCR, cbToWrite);;

		  for (cbWritten = 0; cbWritten < cbToWrite; cbWritten+=2) {
		      WriteData=((*(pbBuffer+1))<<8) | *pbBuffer;	
                    *pulFifoReg = WriteData;
		      pbBuffer +=2;
                }

			
				// We have to consider 16bit buffer interface
                if (pTransfer->cbTransferred == pTransfer->cbBuffer 
					|| cbWritten == 0) {
                    dwStatus = UFN_NO_ERROR;
                    fCompleted = TRUE;
                }

                // By Placing the check for packet complete here, before
                // cbTransferred is updated, there is a 1 interrupt cycle delay
                // That is complete is not declared until the data has actually
                // been ACKd (TPC set) by the host
		  pTransfer->cbTransferred += cbToWrite;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            RETAILMSG(DBG, (_T("%s Exception!\r\n"), pszFname));
            fCompleted = TRUE;
            dwStatus = UFN_CLIENT_BUFFER_ERROR;
        }

        SetProcPermissions(dwCurrentPermissions);
    }   
    else {
        // It is possible for an interrupt to come in while still in this 
        // function for first pass of transfer. If this happens it is possible
        // to complete the transfer and have that interrupt be unnecessary
        // so... just ignore it.
        RETAILMSG(DBG, (_T("%s Not Transfer \r\n"), 
            pszFname));
        goto EXIT;
    }


    if (fCompleted) {
        // Disable transfer interrupts until another transfer is issued.
        if (peps->dwEndpointNumber != 0) {
            DisableEndpointInterrupt(pContext, peps->dwEndpointNumber);
        }

        RETAILMSG(DBG, (_T("%s Tx Done  Ep%x  Status %u\r\n"), pszFname, 
            dwEndpoint, dwStatus));
        CompleteTransfer(pContext, peps, dwStatus);
    }
    else {
        RETAILMSG(DBG, (_T("%s Tx EP%x BufferSize=%u, Xfrd=%u\r\n"), 
            pszFname, dwEndpoint, pTransfer->cbBuffer, pTransfer->cbTransferred));
    }

EXIT:
    FUNCTION_LEAVE_MSG();
#endif
}

#if TEST_MODE_SUPPORT
static BOOL
CheckForUSBTestModeRequest(PCTRLR_PDD_CONTEXT pContext)
{
    bool fTest = FALSE;

	WORD test_temp;

    // is this a request to enter a test mode?
	//RETAILMSG(1, (_T("e\r\n")));

    if( pContext->udr.bmRequestType == (USB_REQUEST_HOST_TO_DEVICE | USB_REQUEST_STANDARD | USB_REQUEST_FOR_DEVICE)
        && pContext->udr.bRequest == USB_REQUEST_SET_FEATURE
        && pContext->udr.wValue == USB_FEATURE_TEST_MODE
        && (pContext->udr.wIndex & 0xFF) == 0) {


               pContext->sendDataEnd = TRUE;

		pContext->Ep0State = EP0_STATE_IDLE;	   


#if 1
		//RETAILMSG(1, (_T("USB_FEATURE_TEST_MODE\r\n")));
			// Set TEST MODE

		Sleep(1);
		test_temp = ReadReg(pContext, TR);
		test_temp |=TR_TMD;
		
		 WriteReg(pContext, TR, test_temp);

		test_temp = ReadReg(pContext, TR);
		
		 USHORT wTestMode = pContext->udr.wIndex >> 8;
		
		switch( wTestMode)
		{
			case USB_TEST_J:

				//RETAILMSG(1, (_T("USB_TEST_J\r\n")));
				//Set Test J
				test_temp |=TR_TJS;
				 WriteReg(pContext, TR, test_temp);
				break;

			case USB_TEST_K:

				//RETAILMSG(1, (_T("USB_TEST_K\r\n")));
				//Set Test K
				test_temp |=TR_TKS;
				 WriteReg(pContext, TR, test_temp);
									
				break;

			case USB_TEST_SE0_NAK:

				//RETAILMSG(1, (_T("USB_TEST_SE0_NAK\r\n")));
				//Set Test SE0NAK
				test_temp |=TR_TSNS;
				 WriteReg(pContext, TR, test_temp);
				
				break;

			case USB_TEST_PACKET:
				
				
					DWORD cbWritten = 0;
				     WORD WriteData = 0;

				volatile ULONG *pulFifoReg = _GetDataRegister(0);

				PBYTE pbBuffer = (PBYTE) ahwTestPkt;

				//RETAILMSG(1, (_T("USB_TEST_PACKET\r\n")));

				// Test Packet Size is 53 Bytes in Spec,, and Our USB FIFO support 2ByteAccess
				//SO TestPacketSize is 53, FIFO Write size is 54 

	  	 		  WriteIndexedReg(pContext, 0, BWCR, TEST_PKT_SIZE);

                for (cbWritten = 0; cbWritten < TEST_ARR_SIZE ; cbWritten++) {
		      WriteData=ahwTestPkt[cbWritten];
		    
                    *pulFifoReg = WriteData;

                }
				test_temp |=TR_TPS;
				 WriteReg(pContext, TR, test_temp);
				
				break;
		}
#endif

		 	fTest = TRUE;
    	}

	return fTest;
}
#endif
 



VOID
HandleEndpoint0Event(
                    PCTRLR_PDD_CONTEXT  pContext
                    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    ValidateContext(pContext);
    DEBUGCHK(pContext->fRunning);

    PEP_STATUS peps = GetEpStatus(pContext, 0);
    LOCK_ENDPOINT(peps);

    ClearEndpointInterrupt(pContext, 0);
    WORD bUSBBusIrqStat = ReadReg(pContext, SSR);
    WORD bEP0IrqStatus = ReadReg(pContext, EP0SR);
    WORD bSCR = ReadReg(pContext, SCR);
    WORD bEP0CR = ReadReg(pContext, EP0CR);	

    // Write 0 to SEND_STALL and SENT_STALL to clear them, so we need to 
    // leave them unchanged by default.
    BYTE bEp0CsrToWrite = 0;



    // Set By USB if protocol violation detected
    if (bEP0IrqStatus & EP0SHT) {
        // Must Clear both Send and Sent Stall
        SetClearReg(pContext, EP0SR, EP0SHT, SET);
        SetClearReg(pContext, EP0CR, EP0ESS, CLEAR);
	 if (bEP0IrqStatus & EP0RSR)
	 	SetClearReg(pContext, EP0SR, EP0RSR, SET);
        pContext->Ep0State = EP0_STATE_IDLE;
    }

    if (bEP0IrqStatus & EP0TST) {
        // Must Clear both Send and Sent Stall
        SetClearReg(pContext, EP0SR, EP0TST, SET);
    }


    BOOL fSendUdr = FALSE;
    
    DWORD cbFifo = ReadIndexedReg(pContext, 0, BRCR);
    BOOL fCompleted = FALSE;
    DWORD dwStatus;

	
	// setup packet
    if (pContext->Ep0State == EP0_STATE_IDLE) {
        if (bEP0IrqStatus & EP0RSR) {
          
            // New setup packet
            const DWORD cbOutFifo = ReadIndexedReg(pContext, 0, BRCR);

		// 16bit Buffer Interface
            PWORD pbUdr = (PWORD) &pContext->udr;
            volatile ULONG *pulFifoReg = _GetDataRegister(0);
            
            DWORD cbBytesRemaining = cbOutFifo;
            while (cbBytesRemaining--) {
                *pbUdr = (WORD) *pulFifoReg;
		  pbUdr ++;
            }
 
	     SetClearReg(pContext, EP0SR, EP0RSR, SET);

#if TEST_MODE_SUPPORT		 
                if(!CheckForUSBTestModeRequest(pContext)){ //for testmode
#else
		if(TRUE){
#endif
		                // Determine if this is a NO Data Packet
		            if (pContext->udr.wLength > 0) {
		                    // Determine transfer Direction
		                    if (pContext->udr.bmRequestType & USB_ENDPOINT_DIRECTION_MASK) {
		                        // Start the SW IN State Machine
		                        pContext->Ep0State = EP0_STATE_IN_DATA_PHASE;     
			                 RETAILMSG(DBG, (_T("%s EP0_STATE_IN_DATA_PHASE\r\n"), pszFname));
		                    }
		                    else {
		                        // Start the SW OUT State Machine
		                        pContext->Ep0State = EP0_STATE_OUT_DATA_PHASE;                         
		                        RETAILMSG(DBG, (_T("%s EP0_STATE_OUT_DATA_PHASE\r\n"), pszFname));                        
		                    }

		                    pContext->sendDataEnd = FALSE;
		            }
		            else { // udr.wLength == 0
		                    // ClientDriver will issue a SendControlStatusHandshake to 
		                    // complete the transaction.
		                    pContext->sendDataEnd = TRUE;

		                    // Nothing left to do... stay in IDLE.
		                    DEBUGCHK(pContext->Ep0State == EP0_STATE_IDLE);
		            }
	                fSendUdr = TRUE;
			}
				
        	}
		else{
			WriteIndexedReg(pContext, 0, EP0SR, bEP0IrqStatus);//for RNDIS Class, 
			}
    }
	// Data Out Packet
    else if (pContext->Ep0State == EP0_STATE_OUT_DATA_PHASE) {
		if (bEP0IrqStatus & EP0RSR) {
			
            // Check For out packet read && receive fifo not empty -> out token event
            if (cbFifo) {
                DEBUGMSG(ZONE_RECEIVE, (_T("%s out token packet on endpoint 0 \r\n"),
                    pszFname));                
                HandleRx(pContext, peps, &fCompleted, &dwStatus);
            }
            // status stage of control transfer; zero-length packet received
            else {                     
                DEBUGMSG(ZONE_RECEIVE, (_T("%s status stage of control transfer on endpoint 0\r\n"),
                    pszFname));
                pContext->Ep0State = EP0_STATE_IDLE;
            	}
           }
	else
		RETAILMSG(DBG, (_T("Not RST in outdataphase: 0x%08x =========\r\n"),bEP0IrqStatus ));     
    }
	// Data In Packet
    else {
		
		HandleTx(pContext, peps, 0);
    }


    if (fCompleted) {
        CompleteTransfer(pContext, peps, dwStatus);
    }

    if (fSendUdr) {
        pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_SETUP_PACKET, (DWORD) &pContext->udr);
    }

   
    FUNCTION_LEAVE_MSG();
    UNLOCK_ENDPOINT(peps);
}

#define DBG_TEST3 1
// Process an endpoint interrupt.  Call interrupt-specific handler.
static
VOID
HandleEndpointEvent(
                    PCTRLR_PDD_CONTEXT  pContext,
                    DWORD               dwEndpoint,
                    DWORD               epIrqStat
                    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    ValidateContext(pContext);
    DEBUGCHK(pContext->fRunning);
    DEBUGCHK(dwEndpoint != 0);

    DWORD dwPendingEvents = 0;
    EP_STATUS *peps = GetEpStatus(pContext, dwEndpoint);
    PREFAST_DEBUGCHK(peps);

    LOCK_ENDPOINT(peps);

    ClearEndpointInterrupt(pContext, dwEndpoint);

    WORD bEpIrqStat;
    bEpIrqStat = ReadIndexedReg(pContext, dwEndpoint, ESR);

    if (peps->dwDirectionAssigned == USB_IN_TRANSFER) {
        // Stall "acknowledged" from Host
        if (bEpIrqStat & FSC) {
            USB_DEVICE_REQUEST udr;
            udr.bmRequestType = USB_REQUEST_FOR_ENDPOINT;
            udr.bRequest = USB_REQUEST_CLEAR_FEATURE;
            udr.wValue = USB_FEATURE_ENDPOINT_STALL;
            udr.wIndex = USB_ENDPOINT_DIRECTION_MASK | (BYTE) dwEndpoint;
            udr.wLength = 0;

            DisableEndpointInterrupt(pContext, dwEndpoint);

            DEBUGMSG(ZONE_PIPE, (_T("%s Got IN_SENT_STALL EP%u \r\n"), 
                pszFname, dwEndpoint));


            pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_SETUP_PACKET, (DWORD) &udr);
            // Must Clear both Send and Sent Stall
            SetClearIndexedReg(pContext, dwEndpoint, ESR, FSC, SET);
        }

	if (bEpIrqStat & TPS) {
        // Must Clear Tx Packet Success
        WriteIndexedReg(pContext, dwEndpoint, ESR, bEpIrqStat);
		dwPendingEvents = IN_TRANSFER;	
	} 

    }
    else {
        // Stall "acknowledged" from Host
        if (bEpIrqStat & FSC) {
            USB_DEVICE_REQUEST udr;
            udr.bmRequestType = USB_REQUEST_FOR_ENDPOINT;
            udr.bRequest = USB_REQUEST_CLEAR_FEATURE;
            udr.wValue = USB_FEATURE_ENDPOINT_STALL;
            PREFAST_SUPPRESS(12006, "Buffer access index expression 'udr.wIndex=(USHORT)dwEndpoint' is being truncated in a shortening cast.");
            udr.wIndex = (USHORT) dwEndpoint;
            udr.wLength = 0;
            
            DisableEndpointInterrupt(pContext, dwEndpoint);

            DEBUGMSG(ZONE_PIPE, (_T("%s Got OUT_SENT_STALL EP%u \r\n"), 
        pszFname, dwEndpoint));
	
	
            pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_SETUP_PACKET, (DWORD) &udr);
            // Must Clear both Send and Sent Stall
            SetClearIndexedReg(pContext, dwEndpoint, ESR, FSC, SET);
        }

	 if (bEpIrqStat & FFS) {
        // Must Clear FIFO Flushed
        SetClearReg(pContext, ESR, FFS, SET);
	 } 
	if (bEpIrqStat & RPS) {
        // Rx Packet Success is automatically cleared when MCU reads all packets
	dwPendingEvents = OUT_TRANSFER;
	 } 
		
    }

    BOOL fCompleted = FALSE;
    DWORD dwStatus;

    if (dwPendingEvents == IN_TRANSFER) {
        HandleTx(pContext, peps, 0); 
    }

    else if (dwPendingEvents == OUT_TRANSFER) {
        HandleRx(pContext, peps, &fCompleted, &dwStatus);
    }

    if (fCompleted) {
        // Disable transfer interrupts until another transfer is issued.
        DisableEndpointInterrupt(pContext, peps->dwEndpointNumber);
	}

    if (fCompleted) {
        CompleteTransfer(pContext, peps, dwStatus);
	}


    FUNCTION_LEAVE_MSG();
    UNLOCK_ENDPOINT(peps);
}






void PrintMsg(UINT32 u32Addr, UINT32 u32Len, UINT8 u8BytesType )
{
//	char u8Fmt[16];
//	char u8Buf[32];
	UINT32 ii;
	int temp;
	UINT8 *p8Addr;
	UINT16 *p16Addr;
	UINT32 *p32Addr;

	NKDbgPrintfW( TEXT("\r\n========== u32Addr=%X, u32Len=%X, uBytes=%X =========="), u32Addr, u32Len,u8BytesType);


	switch(u8BytesType) {
	case 1:
		p8Addr = (UINT8 *)u32Addr;
	for(ii=0;ii<u32Len;ii++) {
		temp = ii*1;
		if( (temp%16)==0 ) {
			NKDbgPrintfW(TEXT("\r\n %08X: "), temp);
		}
		
		NKDbgPrintfW(TEXT("%02X "), p8Addr[ii]);
	}			
		break;
	case 2:
		p16Addr = (UINT16 *)u32Addr;		
	for(ii=0;ii<u32Len;ii++) {
		temp = ii*2;
		if( (temp%16)==0 ) {
			NKDbgPrintfW(TEXT("\r\n %08X: "), temp);
		}	
		NKDbgPrintfW(TEXT("%04X "), p16Addr[ii]);		
	}			

		break;			
	case 4:
	default:
		u8BytesType = 4;
		p32Addr = (UINT32 *)u32Addr;				
		for(ii=0;ii<u32Len;ii++) {
			temp = ii*4;
			if( (temp%16)==0 ) {
				NKDbgPrintfW(TEXT("\r\n %08X: "), temp);
			}		

			NKDbgPrintfW(TEXT("%08X "), p32Addr[ii]);				
		}	
		break;				
	}	
	NKDbgPrintfW( TEXT("\r\n=============================================== \r\n"));	
}


int Send_ShortkeyEvent()
{
	HANDLE SleepShortKeyEvent;	    
	SleepShortKeyEvent=CreateEvent(NULL, FALSE, FALSE, _T("_ATLAS_APP_EVENT"));
	DPNOK(SleepShortKeyEvent);
	if(SleepShortKeyEvent != INVALID_HANDLE_VALUE)
	{	 
		DPSTR("SleepShortKeyEvent");
		SetEventData (SleepShortKeyEvent, 4);
		SetEvent (SleepShortKeyEvent);	
		
		CloseHandle(SleepShortKeyEvent);		
		SleepShortKeyEvent = NULL;
	}
	return 1;
}



static int g_UDiskEvent2AppCnt=0;
 //[david.modify] 2008-10-24 12:37
 // 用来指示U盘图片是否已经显示了
 // 解决拔下充电器，造 成全屏播放VIDEO有问题的BUG
static int g_UDiskBMPShowOn=0;

// Process USB Bus interrupt
VOID
HandleUSBBusIrq(
                    PCTRLR_PDD_CONTEXT  pContext,
                    WORD                bUSBBusIrqStat
                    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

 //[david.modify] 2008-08-06 09:42
 //用来区分usb pc还是usb adapter
//	DPNOK(bUSBBusIrqStat);
	
	 if (bUSBBusIrqStat & VBUSON) {
            SetClearReg(pContext, SSR, VBUSON, SET);          
        }

	 if (bUSBBusIrqStat & SSRINTERR) {
            SetClearReg(pContext, SSR, SSRINTERR, SET);          
        }

	 if (bUSBBusIrqStat & SDE) {
	 	DPSTR("SDE");
            SetClearReg(pContext, SSR, SDE, SET); 
	     if(bUSBBusIrqStat & HSP){
		pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_SPEED,
                     BS_HIGH_SPEED);
		 //RETAILMSG(1, (_T("HIGH Speed\r\n")));
	     }
	     else {
              pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_SPEED,
                     BS_FULL_SPEED);
		 //RETAILMSG(1, (_T("FULL Speed\r\n")));
	     }
        }


        if (bUSBBusIrqStat & HFSUSP) {			
            SetClearReg(pContext, SSR, HFSUSP, SET);
	 	DPSTR("HFSUSP");			
  
		  RETAILMSG(0, (_T("%s Suspend_temp\r\n"), pszFname));	
		  if(pContext->attachedState == UFN_ATTACH)
	                pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_EVENTS, UFN_SUSPEND);
        }

        if (bUSBBusIrqStat & HFRM) {
            SetClearReg(pContext, SSR, HFRM, SET);
	 	DPSTR("HFRM");	

		RETAILMSG(0, (_T("%s Resume_temp\r\n"), pszFname));	

		 if(pContext->attachedState == UFN_ATTACH)
                 pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_EVENTS, UFN_RESUME);
          
        }
		
        if (bUSBBusIrqStat & HFRES) {
            SetClearReg(pContext, SSR, HFRES, SET);

	 	DPSTR("HFRES");	            
            DEBUGMSG(ZONE_USB_EVENTS, (_T("%s Reset\r\n"), pszFname));
            
            pContext->fSpeedReported = FALSE;

	    RETAILMSG(0, (_T("%s Reset\r\n"), pszFname));	
pContext->dwResetCount ++;

#ifdef S805G_USBPWR			
		if (pContext->attachedState == UFN_DETACH && (!(pIOPregs->GPFDAT & (1<<2)))){				
#else
		if (pContext->attachedState == UFN_DETACH && (pIOPregs->GPFDAT & (1<<2))){
#endif			
		RETAILMSG(0, (_T("%s Reset_Attach\r\n"), pszFname));	
		
		if(USBClassInfo == USB_MSF){
		//	Sleep(2000); //for MSF Class. on booting, to generate SD Detect INT and to to mount before storage detectetcion on BOT
//			if (pContext->dwResetCount == 2)
			if(1)
				{
 //[david.modify] 2008-08-26 16:13
 //============================================
 // USBRESET 复位延时一段时间去读USB状态寄存器
 // 如果是USB CABLE，则会存在usb packet往来
 // 如果是其他的如usb adapter,或者usb tmc，则不会有usb packet往来
 //以此区分是USB CABLE还是其他
//            Sleep(1000);	//1s是OK的
            Sleep(500);
	     WORD bEpIrqStat = ReadReg(pContext, EIR);	 
//	     DPNOK3(bEpIrqStat);
//	     PrintMsg((UINT32)g_pUDCBase, 0x100/4, sizeof(UINT32));		
	    if(bEpIrqStat)
	    {
					pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_EVENTS, UFN_ATTACH);
					pContext->attachedState = UFN_ATTACH;

					if((pContext->attachedState==UFN_ATTACH)&&(USBClassInfo == USB_MSF))
					{
						//[david.modify] 2008-07-23 19:36
						DPSTR("SendOvlEvent");
						SendOvlEvent(BITMAP_USBDISK_ID);

						g_UDiskBMPShowOn=1;
						 //[david.modify] 2008-08-16 14:54
						 // 接上U盘后， 发一个消息让AP回到主菜单
						 //======================
						 Send_ShortkeyEvent();
						 //====================== 
 						g_UDiskEvent2AppCnt=1;						
					}					
	    	}					
 //============================================					
					
				}
		}
		else{
                pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_EVENTS, UFN_ATTACH);
                pContext->attachedState = UFN_ATTACH;
			}
            }
        
            
            pContext->Ep0State = EP0_STATE_IDLE;
            pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_EVENTS, UFN_RESET);
        }





		
	
    FUNCTION_LEAVE_MSG();
}


// Process a SC2450 interrupt.

VOID
HandleUSBEvent(
               PCTRLR_PDD_CONTEXT pContext
               )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();
    ValidateContext(pContext);

  
   WORD nStop = FALSE;
    WORD bEpIrqStat = ReadReg(pContext, EIR);
    WORD bUSBBusIrqStat = ReadReg(pContext, SSR);

    WORD bSCR = ReadReg(pContext, SCR);


  //  while (bEpIrqStat || bUSBBusIrqStat) {
        if (bUSBBusIrqStat) {
            HandleUSBBusIrq(pContext, bUSBBusIrqStat);
        }

        if (bEpIrqStat) {

          if (bEpIrqStat & EP0I) {
                HandleEndpoint0Event(pContext);
            }
            else 
            	{
	            // Process All Other (besides EP0) Endpoints
	            for (DWORD dwEndpoint = 1; dwEndpoint < ENDPOINT_COUNT; ++dwEndpoint) {
	                // Check the Interrupt Mask 
	                // Check the Interrupt Status 
	                BYTE bEpBit =  EpToIrqStatBit(dwEndpoint);
	                if (bEpIrqStat & bEpBit) {
	                    HandleEndpointEvent(pContext, dwEndpoint, bEpIrqStat);
				nStop = TRUE; 		
	                }
	   		if(nStop) break;
			}
            	}
        }


    FUNCTION_LEAVE_MSG();
}


/****************************************************************
@doc INTERNAL

@func VOID | SerUSB_InternalMapRegisterAddresses |
This routine maps the ASIC registers. 
It's an artifact of this
implementation.

@rdesc None.
****************************************************************/

DWORD MapRegisterSet(PCTRLR_PDD_CONTEXT pContext)
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    PREFAST_DEBUGCHK(pContext);

    DEBUGCHK(g_pUDCBase == NULL);

    PBYTE   pVMem;
    DWORD   dwRet = ERROR_SUCCESS;

    // Map CSR registers.
    pVMem = (PBYTE)VirtualAlloc(0, PAGE_SIZE, MEM_RESERVE, PAGE_NOACCESS);

    if (pVMem) {
        BOOL fSuccess = VirtualCopy(pVMem, (LPVOID)pContext->dwIOBase,
            pContext->dwIOLen, PAGE_READWRITE | PAGE_NOCACHE);
        if (!fSuccess) {
            VirtualFree(pVMem, 0, MEM_RELEASE);
            dwRet = GetLastError();
            RETAILMSG(DBG, (_T("%s Virtual Copy: FAILED\r\n"), pszFname));
        }
        else {
            g_pUDCBase = pVMem + BASE_REGISTER_OFFSET;

            RETAILMSG(DBG, (_T("%s VirtualCopy Succeeded, pVMem:%x\r\n"), 
                pszFname, pVMem));
        }
    } 
    else {
        dwRet = GetLastError();
        RETAILMSG(DBG, (_T("%s Virtual Alloc: FAILED\r\n"), pszFname));
    }

    FUNCTION_LEAVE_MSG();
    
    return dwRet;
}

/*++

Routine Description:

Deallocate register space.

Arguments:

None.

Return Value:

None.

--*/
static
VOID
UnmapRegisterSet(PCTRLR_PDD_CONTEXT pContext)
{
    // Unmap any memory areas that we may have mapped.
    if (g_pUDCBase) {
        VirtualFree((PVOID) g_pUDCBase, 0, MEM_RELEASE);
        g_pUDCBase = NULL;
    }
}


// interrupt service routine.

DWORD
WINAPI
ISTMain(
        LPVOID lpParameter
        )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) lpParameter;
    ValidateContext(pContext);

    CeSetThreadPriority(pContext->hIST, pContext->dwISTPriority);
	

    while (!pContext->fExitIST) {
        pContext->fRestartIST = FALSE;

        // Disable All Endpoint interrupts
        WriteReg(pContext, EIER, 0); // Disable All

	
	//Resume signal is occured when cable is not connected
	//So, MFRM is setted by '0'

        // Enable Device interrupts
//    SetClearReg(pContext, SCR, (RRDE | HRESE | HSUSPE | MFRM), SET);	
        SetClearReg(pContext, SCR, (RRDE|HRESE|HSUSPE), SET);	

        SetClearReg(pContext, SSR,  (HFRES | HFSUSP | HFRM), SET);

        // Enable Endpoint interrupt 0
        EnableEndpointInterrupt(pContext, 0);

        while (TRUE) {
            DWORD dwWait = WaitForSingleObject(pContext->hevInterrupt, INFINITE);
            if (pContext->fExitIST || pContext->fRestartIST) {
                break;
            }

            if (dwWait == WAIT_OBJECT_0) {
                HandleUSBEvent(pContext);
                InterruptDone(pContext->dwSysIntr);
            }
            else {
                RETAILMSG(DBG, (_T("%s WaitForMultipleObjects failed. Exiting IST.\r\n"), 
                    pszFname));
                break;
            }
        }
        // Send detach
        pContext->pfnNotify(pContext->pvMddContext, 
            UFN_MSG_BUS_EVENTS, UFN_DETACH);

        pContext->fSpeedReported = FALSE;
        pContext->attachedState = UFN_DETACH;

        // Disable Device  interrupts - write Zeros to Disable
        SetClearReg(pContext, SCR, (HRESE | HSUSPE | MFRM), CLEAR);

        // Disable endpoint interrupts - write Zeros to Disable
        WriteReg(pContext, EIER, 0);
        
        // Clear any outstanding device & endpoint interrupts
        // USB Device Interrupt Status - Write a '1' to Clear 
        SetClearReg(pContext, SSR,  (HFRES | HFSUSP | HFRM), SET);
        // End point Interrupt Status - Write a '1' to Clear
        WriteReg(pContext, EIR, CLEAR_ALL_EP_INTRS);



    }

    FUNCTION_LEAVE_MSG();

    return 0;
}


// EINT2 Test Code by woo
static DWORD 
PLUG_IST(LPVOID lpParameter)
{
//DWORD i;

	PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) lpParameter;
       ValidateContext(pContext);

    while (1) {
        __try {
            WaitForSingleObject(g_PlugEvent, INFINITE);
#ifdef S805G_USBPWR			
			if (!( pIOPregs->GPFDAT & (1<<2) ))
#else
			if ( pIOPregs->GPFDAT & (1<<2) )
#endif
			{
				// Set the Normal mode
#ifdef S805G_USBPWR
 				g_stGPIOInfo[0].u32Stat=1;
				SetGPIOInfo(&g_stGPIOInfo[0],  (void*)pIOPregs);	



				pCLKPWR->USB_RSTCON = (0<<2)|(0<<1)|(1<<0);
				Sleep(2);
				pCLKPWR->USB_RSTCON = (0<<2)|(0<<1)|(0<<0);
				pCLKPWR->USB_CLKCON = (1<<31)|(1<<2)|(0<<1)|(0<<0);//pull up enable
 //[david.modify] 2008-09-02 18:33
 //=====================================================
//				pCLKPWR->USB_PHYCTRL = (0<<3)|(1<<2)|(0<<1)|(0<<0);
				// 无源晶振使用如下:Crystal (新板)
				pCLKPWR->USB_PHYCTRL = (2<<3)|(0<<2)|(0<<1)|(0<<0);		
 //===================================================== 
				pCLKPWR->USB_PHYPWR = (0<<31)|(3<<4)|(0<<3)|(0<<2)|(0<<1)|(0<<0);
#else
				pIOPregs->GPHCON = (pIOPregs->GPHCON & ~(0x3<<28)) | (0x1<<28);
			    	pIOPregs->GPHUDP = (pIOPregs->GPHUDP & ~(0x3<<28));
			       pIOPregs->GPHDAT = (pIOPregs->GPHDAT & ~(0x1<<14)) | (0x1<<14);
			   
#endif
			   
#if (BSP_TYPE == BSP_SMDK2443)
				// Enable the PHY Power	
				pCLKPWR->PWRCFG |= (1<<4);
				pCLKPWR->USB_CLKCON = (0<<31)|(1<<2)|(0<<1)|(0<<0); //pullup disable	for phy,func reset
				// First USB PHY has to be reset, and after 10us, func and host has to be reset.
				pCLKPWR->USB_RSTCON = (0<<2)|(0<<1)|(1<<0);
				Sleep(1);
				pCLKPWR->USB_RSTCON = (1<<2)|(0<<1)|(0<<0);
				pCLKPWR->USB_RSTCON = (0<<2)|(0<<1)|(0<<0);
				pCLKPWR->USB_CLKCON = (1<<31)|(1<<2)|(0<<1)|(0<<0); //pullup enable	
#elif (BSP_TYPE == BSP_SMDK2450)
#endif
				pContext->dwResetCount = 0;

				RETAILMSG(1,(TEXT("+USB Cable Plug in\r\n")));
			}
			else
			{

				// Set the Normal mode
#ifdef S805G_USBPWR
 				g_stGPIOInfo[0].u32Stat=0;
				SetGPIOInfo(&g_stGPIOInfo[0],  (void*)pIOPregs);	
#else				
			       pIOPregs->GPHCON = (pIOPregs->GPHCON & ~(0x3<<28)) | (0x1<<28);
			       pIOPregs->GPHUDP = (pIOPregs->GPHUDP & ~(0x3<<28)) | (0x2<<28);      
		     	       pIOPregs->GPHDAT = (pIOPregs->GPHDAT & ~(0x1<<14));  
#endif
				
    	 	             pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_EVENTS, UFN_DETACH);
   		             pContext->attachedState = UFN_DETACH;
				pContext->dwResetCount = 0;

				//[david.modify] 2008-07-23 19:36
				DPNOK(USBClassInfo);
				if(USBClassInfo == USB_MSF)
//				if(1)
				{
					if(1==g_UDiskBMPShowOn) {
						SendOvlEvent(BITMAP_HIDE_ID);
						g_UDiskEvent2AppCnt=0;
					}
				}					 

				RETAILMSG(1,(TEXT("-USB Cable Plug out\r\n")));
			}
			
			InterruptDone(g_PlugSysIntr);
        }
	__except(EXCEPTION_EXECUTE_HANDLER) {
            RETAILMSG(1,(TEXT("!!! PLUG_IST EXCEPTION: 0x%X !!!\r\n"), GetExceptionCode() ));
	}
    	}

	return 0;
}

static DWORD 
SDCARD_DetectIST(LPVOID lpParameter)
{
#if 1
	PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) lpParameter;
       ValidateContext(pContext);

	
    while (1) {
        __try {
            WaitForSingleObject(v_gBspArgs->g_SDCardDetectEvent, INFINITE); 

	            if(bSDMMCMSF)
	            	{
				RETAILMSG(0,(TEXT("CardState :%d, AttachState : %d\r\n"),v_gBspArgs->g_SDCardState,pContext->attachedState));
#ifdef S805G_USBPWR			
			if (!( pIOPregs->GPFDAT & (1<<2) ))
#else
			if ( pIOPregs->GPFDAT & (1<<2) )
#endif
					{ //cable connection矫父 

				/*	if(v_gBspArgs->g_SDCardState== CARD_INSERTED){
						RETAILMSG(1,(TEXT("SetEvent: PlugEvent(CARD_INSERTED)\r\n")));

#ifdef S805G_USBPWR
 				g_stGPIOInfo[0].u32Stat=1;
				SetGPIOInfo(&g_stGPIOInfo[0],  (void*)pIOPregs);	
#else
						pIOPregs->GPHCON = (pIOPregs->GPHCON & ~(0x3<<28)) | (0x1<<28);
					    	pIOPregs->GPHUDP = (pIOPregs->GPHUDP & ~(0x3<<28));
					       pIOPregs->GPHDAT = (pIOPregs->GPHDAT & ~(0x1<<14)) | (0x1<<14);
#endif						   

#if (BSP_TYPE == BSP_SMDK2443)
						pCLKPWR->USB_CLKCON = (0<<31)|(1<<2)|(0<<1)|(0<<0); //pullup disable	for phy,func reset
						// First USB PHY has to be reset, and after 10us, func and host has to be reset.
						pCLKPWR->USB_RSTCON = (0<<2)|(0<<1)|(1<<0);
						Sleep(1);
						pCLKPWR->USB_RSTCON = (1<<2)|(0<<1)|(0<<0);
						pCLKPWR->USB_RSTCON = (0<<2)|(0<<1)|(0<<0);
						pCLKPWR->USB_CLKCON = (1<<31)|(1<<2)|(0<<1)|(0<<0); //pullup enable	
#elif (BSP_TYPE == BSP_SMDK2450)
#endif

						}
					else if(v_gBspArgs->g_SDCardState== CARD_REMOVED){
						RETAILMSG(1,(TEXT("SetEvent: PlugEvent(CARD_REMOVED)\r\n")));
#ifdef S805G_USBPWR
 				g_stGPIOInfo[0].u32Stat=0;
				SetGPIOInfo(&g_stGPIOInfo[0],  (void*)pIOPregs);	
#else				
		 				 pIOPregs->GPHCON = (pIOPregs->GPHCON & ~(0x3<<28)) | (0x1<<28);
					       pIOPregs->GPHUDP = (pIOPregs->GPHUDP & ~(0x3<<28)) | (0x2<<28);      
				     	       pIOPregs->GPHDAT = (pIOPregs->GPHDAT & ~(0x1<<14));  
#endif
		    	 	             pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_EVENTS, UFN_DETACH);
		   		             pContext->attachedState = UFN_DETACH;

			
						}
					else
						RETAILMSG(1,(TEXT("SetEvent: PlugEvent(else)\r\n")));*/
							RETAILMSG(1,(TEXT("Wait a few(5~10)sec until rescanning mass storage device!\r\n")));
#ifdef S805G_USBPWR
 //				g_stGPIOInfo[0].u32Stat=0;
//				SetGPIOInfo(&g_stGPIOInfo[0],  (void*)pIOPregs);	
				pCLKPWR->USB_CLKCON = (0<<31)|(1<<2)|(0<<1)|(0<<0); //pullup disable
#else				
		 				 pIOPregs->GPHCON = (pIOPregs->GPHCON & ~(0x3<<28)) | (0x1<<28);
					       pIOPregs->GPHUDP = (pIOPregs->GPHUDP & ~(0x3<<28)) | (0x2<<28);      
				     	       pIOPregs->GPHDAT = (pIOPregs->GPHDAT & ~(0x1<<14));  
#endif
							
							pContext->pfnNotify(pContext->pvMddContext, UFN_MSG_BUS_EVENTS, UFN_DETACH);
							pContext->attachedState = UFN_DETACH;
							pContext->dwResetCount = 0;
							Sleep(2000);
							
#ifdef S805G_USBPWR
 //				g_stGPIOInfo[0].u32Stat=1;
//				SetGPIOInfo(&g_stGPIOInfo[0],  (void*)pIOPregs);	
				pCLKPWR->USB_CLKCON = (1<<31)|(1<<2)|(0<<1)|(0<<0); //pullup enable
#else
						pIOPregs->GPHCON = (pIOPregs->GPHCON & ~(0x3<<28)) | (0x1<<28);
					    	pIOPregs->GPHUDP = (pIOPregs->GPHUDP & ~(0x3<<28));
					       pIOPregs->GPHDAT = (pIOPregs->GPHDAT & ~(0x1<<14)) | (0x1<<14);
#endif						   

							
						
					}
				else
					RETAILMSG(1,(TEXT("SetEvent: PlugEvent_ Cable not Connected\r\n")));
				}
				else
					RETAILMSG(1,(TEXT("SetEvent: PlugEvent_ Not SDMMC\r\n")));	
			}
	__except(EXCEPTION_EXECUTE_HANDLER) {
            RETAILMSG(1,(TEXT("!!! SDCARD_DetectTherad: 0x%X !!!\r\n"), GetExceptionCode() ));
	}
    	}


#endif
	return 0;
}

static DWORD 
SendEvent2App_IST(LPVOID lpParameter)
{
	Sleep(5000);	//等待5s钟后发送消息
	DPNOK(g_UDiskEvent2AppCnt);
	if(g_UDiskEvent2AppCnt>=1) {
			//[david.modify] 2008-07-23 19:36
			DPSTR("SendOvlEvent");
			SendOvlEvent(BITMAP_USBDISK_ID);
						g_UDiskBMPShowOn=1;			
			 //[david.modify] 2008-08-16 14:54
			 // 接上U盘后， 发一个消息让AP回到主菜单
			 //======================
			 Send_ShortkeyEvent();
			 //====================== 
	}
	return 0;
}

// end woo

static
VOID
StartTransfer(
              PCTRLR_PDD_CONTEXT pContext,
              PEP_STATUS peps,
              PSTransfer pTransfer
              )
{
    SETFNAME();

    DEBUGCHK(pContext);
    PREFAST_DEBUGCHK(peps);

    DEBUGCHK(!peps->pTransfer);
    ValidateTransferDirection(pContext, peps, pTransfer);

    LOCK_ENDPOINT(peps);
    FUNCTION_ENTER_MSG();

    RETAILMSG(DBG, (_T("%s Setting up %s transfer on ep %u for %u bytes\r\n"),
        pszFname, (pTransfer->dwFlags == USB_IN_TRANSFER) ? _T("in") : _T("out"),
        peps->dwEndpointNumber, pTransfer->cbBuffer));

    // Enable transfer interrupts.
    peps->pTransfer = pTransfer;
    DWORD dwEndpoint = peps->dwEndpointNumber;

    if (pTransfer->dwFlags == USB_IN_TRANSFER) {

		if(pTransfer->cbBuffer == 0){ // for rndis, zero length packet
			WriteIndexedReg(pContext, dwEndpoint, BWCR, 0);
			RETAILMSG(DBG, (_T("%s  zero length packet\r\n"),
            pszFname));

			}
			else {
        if (peps->dwEndpointNumber == 0) {

			WORD bEp0ESR = ReadReg(pContext, EP0SR);
			if (bEp0ESR & EP0TST) WriteReg(pContext, EP0SR, bEp0ESR);
			
			HandleTx(pContext, peps, 0);
        }
        else {
			WORD bEpESR = ReadIndexedReg(pContext, dwEndpoint, ESR);
			if (bEpESR & TPS) {
				WriteIndexedReg(pContext, dwEndpoint, ESR, bEpESR);
			}
		HandleTx(pContext, peps, 1);
	        }
        }
    }
    else {
        if (peps->dwEndpointNumber == 0) {

        }
        else {
            EnableEndpointInterrupt(pContext, peps->dwEndpointNumber);

            // There may be a packet available.  If so process it...
            WORD bEpIrqStat = ReadIndexedReg(pContext, peps->dwEndpointNumber, ESR);
            
            if (bEpIrqStat & RPS) {
                BOOL fCompleted;
                DWORD dwStatus;
                HandleRx(pContext, peps, &fCompleted, &dwStatus);

                if (fCompleted) {
                    // Disable transfer interrupts until another transfer is issued.
                    DisableEndpointInterrupt(pContext, peps->dwEndpointNumber);
                }

              
                if (fCompleted) {
                    CompleteTransfer(pContext, peps, dwStatus);
                }    
            }
        }
    }
    
    FUNCTION_LEAVE_MSG();

    UNLOCK_ENDPOINT(peps);
}
DWORD
WINAPI
UfnPdd_IssueTransfer(
    PVOID  pvPddContext,
    DWORD  dwEndpoint,
    PSTransfer pTransfer
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    DEBUGCHK(EP_VALID(dwEndpoint));

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    PEP_STATUS peps = GetEpStatus(pContext, dwEndpoint);
    LOCK_ENDPOINT(peps);
    DEBUGCHK(peps->fInitialized);
    DEBUGCHK(pTransfer->cbTransferred == 0);

    DWORD dwRet = ERROR_SUCCESS;

    RETAILMSG(DBG, (_T("%s  UfnPdd_IssueTransfer\r\n"),
            pszFname));

    // Note For the HW NAKs IN requests and DOES NOT let SW
    // know that the Host is trying to send a request. SO... Start the Transfer 
    // In Now!
    // Start the Transfer
    DEBUGCHK(peps->pTransfer == NULL);
    StartTransfer(pContext, peps, pTransfer);

    UNLOCK_ENDPOINT(peps);

    FUNCTION_LEAVE_MSG();

    return dwRet;
}


DWORD
WINAPI
UfnPdd_AbortTransfer(
    PVOID           pvPddContext,
    DWORD           dwEndpoint,
    PSTransfer      pTransfer
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    PREFAST_DEBUGCHK(pTransfer);
    DEBUGCHK(EP_VALID(dwEndpoint));

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    PEP_STATUS peps = GetEpStatus(pContext, dwEndpoint);
    LOCK_ENDPOINT(peps);
    DEBUGCHK(peps->fInitialized);

	RETAILMSG(DBG, (_T("%s  UfnPdd_AbortTransfer\r\n"),
            pszFname));

    ValidateTransferDirection(pContext, peps, pTransfer);

    DEBUGCHK(pTransfer == peps->pTransfer);
    CompleteTransfer(pContext, peps, UFN_CANCELED_ERROR);

    if (dwEndpoint == 0) {
        pContext->Ep0State = EP0_STATE_IDLE;
    }
    
    ResetEndpoint( pContext,peps);

    UNLOCK_ENDPOINT(peps);

    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}


BOOL HW_USBClocks(CEDEVICE_POWER_STATE    cpsNew)
{
	if (cpsNew == D0) //power on
	{
	
		RETAILMSG(0, (TEXT("HW_USBClocks::D0 \r\n")));
//		if(pIOPregs->GPFDAT & (1<<2)) //check cable connection, for powerconsumption
#ifdef S805G_USBPWR			
//			if (!( pIOPregs->GPFDAT & (1<<2) ))
#else
//			if ( pIOPregs->GPFDAT & (1<<2) )
#endif
		{
		        // Set the Normal mode
#ifdef S805G_USBPWR
 				g_stGPIOInfo[0].u32Stat=1;
				SetGPIOInfo(&g_stGPIOInfo[0],  (void*)pIOPregs);	
#else		        
		    pIOPregs->GPHCON = (pIOPregs->GPHCON & ~(0x3<<28)) | (0x1<<28);
		    pIOPregs->GPHUDP = (pIOPregs->GPHUDP & ~(0x3<<28));
		    pIOPregs->GPHDAT = (pIOPregs->GPHDAT & ~(0x1<<14)) | (0x1<<14);
#endif			
			pIOPregs->MISCCR &= ~(1<<12);
			// Enable the PHY Power	
			pCLKPWR->PWRCFG |= (1<<4);
			// First USB PHY has to be reset, and after 10us, func and host has to be reset.

			pCLKPWR->USB_CLKCON = (0<<31)|(1<<2)|(0<<1)|(0<<0); //pullup disable
			
			pCLKPWR->USB_RSTCON = (0<<2)|(0<<1)|(1<<0);
			Sleep(2);
			pCLKPWR->USB_RSTCON = (1<<2)|(0<<1)|(0<<0);
			Sleep(2);
			pCLKPWR->USB_RSTCON = (0<<2)|(0<<1)|(0<<0);
			pCLKPWR->USB_CLKCON = (1<<31)|(1<<2)|(0<<1)|(0<<0);//pull up enable

//[david.modify] 2008-05-08 17:28
// 我们使用12MHZ晶振替换SMDK2450的48MHZ晶振
//========================================
 //[david.modify] 2008-05-08 17:28
// 我们使用12MHZ晶振替换SMDK2450的48MHZ晶振
//	pCLKPWR->USB_PHYCTRL = (0<<3)|(1<<2)|(0<<1)|(0<<0);

// 有源晶振使用如下:Oscillator
//	pCLKPWR->USB_PHYCTRL = (2<<3)|(1<<2)|(0<<1)|(0<<0);

// 无源晶振使用如下:Crystal (新板)
	pCLKPWR->USB_PHYCTRL = (2<<3)|(0<<2)|(0<<1)|(0<<0);		
 //========================================
			// Set USB PHY Power
			// 48Mhz clock on ,PHY2.0 analog block power on,XO block power on,
			// XO block power in suspend mode,PHY 2.0 Pll power on ,suspend signal for save mode disable
			pCLKPWR->USB_PHYPWR = (0<<31)|(3<<4)|(0<<3)|(0<<2)|(0<<1)|(0<<0);
			// Set USB Clock Control
			// Vbus detect enable and D+ Pull up, USB2.0 Function clock Enable, 
			// USB1.1 HOST disable,USB2.0 PHY test enable

			}
	}
	// Should be implemented
	else if (cpsNew == D4)  //power off
	{
		RETAILMSG(0, (TEXT("HW_USBClocks::D4 \r\n")));



		pCLKPWR->USB_CLKCON = (0<<31)|(0<<2)|(0<<1)|(0<<0); //pullup disable
		// disable the PHY Power	
		pCLKPWR->PWRCFG &= ~(1<<4);


		// Set the Normal mode regulator disable
#ifdef S805G_USBPWR
 				g_stGPIOInfo[0].u32Stat=0;
				SetGPIOInfo(&g_stGPIOInfo[0],  (void*)pIOPregs);	
#else		
		pIOPregs->GPHCON = (pIOPregs->GPHCON & ~(0x3<<28)) | (0x1<<28);
		pIOPregs->GPHUDP = (pIOPregs->GPHUDP & ~(0x3<<28)) | (0x2<<28);      
		pIOPregs->GPHDAT = (pIOPregs->GPHDAT & ~(0x1<<14));  
#endif		
		pIOPregs->MISCCR |= (1<<12);
}

	return TRUE;
}

// This does not do much because there is not any way to control
// power on this controller.
static
CEDEVICE_POWER_STATE
SetPowerState(
    PCTRLR_PDD_CONTEXT      pContext,
    CEDEVICE_POWER_STATE    cpsNew
    )
{
    SETFNAME();
    
    PREFAST_DEBUGCHK(pContext);
    DEBUGCHK(VALID_DX(cpsNew));
    ValidateContext(pContext);

RETAILMSG(DBG, (_T("%s  SetPowerState\r\n"),
            pszFname));

    // Adjust cpsNew.
    if (cpsNew != pContext->cpsCurrent) {
        if (cpsNew == D1 || cpsNew == D2) {
            // D1 and D2 are not supported.
            cpsNew = D0;
        }
        else if (pContext->cpsCurrent == D4) {
            // D4 can only go to D0.
            cpsNew = D0;
        }
    }

    if (cpsNew != pContext->cpsCurrent) {
        DEBUGMSG(ZONE_POWER, (_T("%s Going from D%u to D%u\r\n"),
            pszFname, pContext->cpsCurrent, cpsNew));
        
        if ( (cpsNew < pContext->cpsCurrent) && pContext->hBusAccess ) {
            SetDevicePowerState(pContext->hBusAccess, cpsNew, NULL);
        }

        switch (cpsNew) {
        case D0:
		HW_USBClocks(D0);												
            KernelIoControl(IOCTL_HAL_DISABLE_WAKE, &pContext->dwSysIntr,  
                sizeof(pContext->dwSysIntr), NULL, 0, NULL);
            
            if (pContext->fRunning) {
                // Cause the IST to restart.
                pContext->fRestartIST = TRUE;
                SetInterruptEvent(pContext->dwSysIntr);
            }           
            break;

        case D3:
            KernelIoControl(IOCTL_HAL_ENABLE_WAKE, &pContext->dwSysIntr,  
                sizeof(pContext->dwSysIntr), NULL, 0, NULL);
            break;

        case D4:
			HW_USBClocks((CEDEVICE_POWER_STATE)D4);													
            KernelIoControl(IOCTL_HAL_DISABLE_WAKE, &pContext->dwSysIntr,  
                sizeof(pContext->dwSysIntr), NULL, 0, NULL);
            break;
        }

        if ( (cpsNew > pContext->cpsCurrent) && pContext->hBusAccess ) {
            SetDevicePowerState(pContext->hBusAccess, cpsNew, NULL);
        }

		pContext->cpsCurrent = cpsNew;
    }

    return pContext->cpsCurrent;
}

// EINT2 Test Code by woo
DWORD
HW_InitRegisters()
{


#if 1
	v_gBspArgs = (volatile BSP_ARGS *)VirtualAlloc(0, sizeof(BSP_ARGS), MEM_RESERVE, PAGE_NOACCESS);
	if (!v_gBspArgs)
	{
		RETAILMSG(1, (TEXT("#####:common Argument Area  VirtualAlloc failed!\r\n")));
		return FALSE; 
	}
	if (!VirtualCopy((PVOID)v_gBspArgs, (PVOID)(IMAGE_SHARE_ARGS_UA_START), sizeof(BSP_ARGS), PAGE_READWRITE | PAGE_NOCACHE ))
	{
		RETAILMSG(1, (TEXT("#####::BSP Common Argument Are VirtualCopy failed!\r\n")));
		return FALSE;
	}
	pCLKPWR = (volatile S3C2450_CLKPWR_REG*)VirtualAlloc(0, sizeof(S3C2450_CLKPWR_REG), MEM_RESERVE, PAGE_NOACCESS);
	if (!pCLKPWR)
	{
		DEBUGMSG(1, (TEXT("pCLKPWR: VirtualAlloc failed!\r\n")));
		return(FALSE);
	}
	if (!VirtualCopy((PVOID)pCLKPWR, (PVOID)(S3C2450_BASE_REG_PA_CLOCK_POWER >> 8), sizeof(S3C2450_CLKPWR_REG), PAGE_PHYSICAL | PAGE_READWRITE | PAGE_NOCACHE))
	{
		DEBUGMSG(1, (TEXT("pCLKPWR: VirtualCopy failed!\r\n")));
		return(FALSE);
	}

	pIOPregs = (volatile S3C2450_IOPORT_REG *)VirtualAlloc(0, sizeof(S3C2450_IOPORT_REG), MEM_RESERVE, PAGE_NOACCESS);
	if(!pIOPregs) {
		DEBUGMSG(1, (TEXT("pIOPregs: VirtualAlloc failed!\r\n")));
		return(FALSE);
	}
	if(!VirtualCopy((PVOID)pIOPregs, (PVOID)(S3C2450_BASE_REG_PA_IOPORT >> 8), sizeof(S3C2450_IOPORT_REG), PAGE_PHYSICAL | PAGE_READWRITE | PAGE_NOCACHE)) 
	{
		DEBUGMSG(1, (TEXT("pIOPregs: VirtualCopy failed!\r\n")));
		return(FALSE);
	}
#endif
#if (BSP_TYPE == BSP_SMDK2443)
	pIOPregs->EXTINT0 = (READEXTINT0(pIOPregs->EXTINT0) & ~(0xf<<8)) | (1<<11) | (7<<8);
	pIOPregs->GPFCON &= ~(3<<4);
	pIOPregs->GPFCON |= (2<<4);

#elif (BSP_TYPE == BSP_SMDK2450)
	pIOPregs->EXTINT0 = (pIOPregs->EXTINT0 & ~(0xf<<8)) | (0<<11) | (7<<8); //filter enable, 
	pIOPregs->GPFCON &= ~(3<<4);
	pIOPregs->GPFCON |= (2<<4);


	pIOPregs->GPFUDP&= ~(0x3 << 4);		// Pull up down disable
	pIOPregs->GPFUDP|=  (0x0 << 4);
#endif


// end woo


    return 0;
}
// end woo


static
VOID
FreeCtrlrContext(
    PCTRLR_PDD_CONTEXT pContext
    )
{
    PREFAST_DEBUGCHK(pContext);
    DEBUGCHK(!pContext->hevInterrupt);
    DEBUGCHK(!pContext->hIST);
    DEBUGCHK(!pContext->fRunning);

    pContext->dwSig = GARBAGE_DWORD;

    UnmapRegisterSet(pContext);

    if (pContext->hBusAccess) CloseBusAccessHandle(pContext->hBusAccess);
    
    if (pContext->dwSysIntr) {
        KernelIoControl(IOCTL_HAL_DISABLE_WAKE, &pContext->dwSysIntr,  
    	    sizeof(pContext->dwSysIntr), NULL, 0, NULL);
    }

    if (pContext->dwIrq != IRQ_UNSPECIFIED) {
        KernelIoControl(IOCTL_HAL_RELEASE_SYSINTR, &pContext->dwSysIntr, 
            sizeof(DWORD), NULL, 0, NULL);
    }
    
    DeleteCriticalSection(&pContext->csIndexedRegisterAccess);

    LocalFree(pContext);
}


DWORD
WINAPI
UfnPdd_Deinit(
    PVOID pvPddContext
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

	RETAILMSG(DBG, (_T("%s  UfnPdd_Deinit\r\n"),
            pszFname));

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    FUNCTION_ENTER_MSG();

    FreeCtrlrContext(pContext);

    InterruptDisable(g_PlugSysIntr);
    SetEvent(g_PlugEvent);
    WaitForSingleObject(g_PlugThread, INFINITE);
    CloseHandle(g_PlugEvent);
   CloseHandle(g_PlugThread);   
   CloseHandle(g_SdCardDetectThread);
   CloseHandle(g_tmpThread);   
    g_PlugThread = NULL;
    g_PlugEvent = NULL;
   g_SdCardDetectThread = NULL;	
g_tmpThread = NULL;



	
	if (v_gBspArgs)
    {
	VirtualFree((PVOID) v_gBspArgs, 0, MEM_RELEASE);	
        v_gBspArgs = NULL;
    }
	
    	if (pIOPregs)
    {
        VirtualFree((PVOID)pIOPregs, 0, MEM_RELEASE);
        pIOPregs = NULL;
    }
    if (pCLKPWR)
    {   
        VirtualFree((PVOID)pCLKPWR, 0, MEM_RELEASE);
        pCLKPWR = NULL;
    } 
    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}


DWORD
WINAPI
UfnPdd_Start(
    PVOID        pvPddContext
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    DWORD dwRet;
    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    FUNCTION_ENTER_MSG();

    DEBUGCHK(!pContext->fRunning);

    BOOL fIntInitialized = FALSE;


    // Create the interrupt event
    pContext->hevInterrupt = CreateEvent(0, FALSE, FALSE, NULL);
    if (pContext->hevInterrupt == NULL) {
        dwRet = GetLastError();
        RETAILMSG(DBG, (_T("%s Error creating  interrupt event. Error = %d\r\n"), 
            pszFname, dwRet));
        goto EXIT;
    }

    fIntInitialized = InterruptInitialize(pContext->dwSysIntr, 
        pContext->hevInterrupt, NULL, 0);
    if (fIntInitialized == FALSE) {
        dwRet = ERROR_GEN_FAILURE;
        RETAILMSG(DBG, (_T("%s  interrupt initialization failed\r\n"),
            pszFname));
        goto EXIT;
    }
    InterruptDone(pContext->dwSysIntr);

    pContext->fExitIST = FALSE;
    pContext->hIST = CreateThread(NULL, 0, ISTMain, pContext, 0, NULL);    
    if (pContext->hIST == NULL) {
        RETAILMSG(DBG, (_T("%s IST creation failed\r\n"), pszFname));
        dwRet = GetLastError();
        goto EXIT;
    }


    pContext->fRunning = TRUE;
    dwRet = ERROR_SUCCESS;

EXIT:
    if (pContext->fRunning == FALSE) {
        DEBUGCHK(dwRet != ERROR_SUCCESS);
        if (fIntInitialized) InterruptDisable(pContext->dwSysIntr);
        if (pContext->hevInterrupt) CloseHandle(pContext->hevInterrupt);
        pContext->hevInterrupt = NULL;
    }

    FUNCTION_LEAVE_MSG();

    return dwRet;
}


// Stop the device.
DWORD
WINAPI
UfnPdd_Stop(
            PVOID pvPddContext
            )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

RETAILMSG(DBG, (_T("%s  UfnPdd_Stop\r\n"),
            pszFname));
            
    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    DEBUGCHK(pContext->fRunning);

    // Stop the IST
    pContext->fExitIST = TRUE;
    InterruptDisable(pContext->dwSysIntr);
    SetEvent(pContext->hevInterrupt);
    WaitForSingleObject(pContext->hIST, INFINITE);
    CloseHandle(pContext->hevInterrupt);
    CloseHandle(pContext->hIST);
    pContext->hIST = NULL;
    pContext->hevInterrupt = NULL;



    ResetDevice(pContext);

    pContext->fRunning = FALSE;


// end woo

    DEBUGMSG(ZONE_FUNCTION, (_T("%s Device has been stopped\r\n"), 
        pszFname));

    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}


DWORD 
WINAPI 
UfnPdd_IsConfigurationSupportable(
    PVOID                       pvPddContext,
    UFN_BUS_SPEED               Speed,
    PUFN_CONFIGURATION          pConfiguration
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

	RETAILMSG(DBG, (_T("%s  UfnPdd_IsConfigurationSupportable\r\n"),
            pszFname));

    DEBUGCHK((Speed == BS_FULL_SPEED) | (Speed == BS_HIGH_SPEED));

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    // This PDD does not have any special requirements that cannot be 
    // handled through IsEndpointSupportable.
    DWORD dwRet = ERROR_SUCCESS;

    FUNCTION_LEAVE_MSG();

    return dwRet;
}


// 
// Is this endpoint supportable.
DWORD
WINAPI
UfnPdd_IsEndpointSupportable(
    PVOID                       pvPddContext,
    DWORD                       dwEndpoint,
    UFN_BUS_SPEED               Speed,
    PUSB_ENDPOINT_DESCRIPTOR    pEndpointDesc,
    BYTE                        bConfigurationValue,
    BYTE                        bInterfaceNumber,
    BYTE                        bAlternateSetting
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

RETAILMSG(DBG, (_T("%s  UfnPdd_IsEndpointSupportable\r\n"),
            pszFname));


//RETAILMSG(1, (_T("%s  UfnPdd_IsEndpointSupportable\r\n"),
   //         pszFname));

    DEBUGCHK(EP_VALID(dwEndpoint));
    DEBUGCHK((Speed == BS_FULL_SPEED) | (Speed == BS_HIGH_SPEED));

    DWORD dwRet = ERROR_SUCCESS;
    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    PEP_STATUS peps = GetEpStatus(pContext, dwEndpoint);

    // Special case for endpoint 0
    if (dwEndpoint == 0) {
        DEBUGCHK(pEndpointDesc->bmAttributes == USB_ENDPOINT_TYPE_CONTROL);
        
        // Endpoint 0 only supports 8 or 16 byte packet size
        if (pEndpointDesc->wMaxPacketSize < EP_0_PACKET_SIZE) {
            RETAILMSG(DBG, (_T("%s Endpoint 0 only supports %u byte packets\r\n"),
                pszFname, EP_0_PACKET_SIZE));
            dwRet = ERROR_INVALID_PARAMETER;
        }
        else{  
            // Larger than EP 0 Max Packet Size - reduce to Max
            pEndpointDesc->wMaxPacketSize = EP_0_PACKET_SIZE;
        }
    }
    else if (dwEndpoint < ENDPOINT_COUNT) {
        BYTE bTransferType = pEndpointDesc->bmAttributes & USB_ENDPOINT_TYPE_MASK;
        DEBUGCHK(bTransferType != USB_ENDPOINT_TYPE_CONTROL);

        // Validate and adjust packet size
        WORD wPacketSize = 
            (pEndpointDesc->wMaxPacketSize & USB_ENDPOINT_MAX_PACKET_SIZE_MASK);

        switch(bTransferType) {

        // Isoch not currently supported by Samsung HW
        case USB_ENDPOINT_TYPE_ISOCHRONOUS:
            RETAILMSG(DBG, (_T("%s Isochronous endpoints are not supported\r\n"),
                pszFname));
            dwRet = ERROR_INVALID_PARAMETER;
            break;

        case USB_ENDPOINT_TYPE_BULK:
        case USB_ENDPOINT_TYPE_INTERRUPT:
            // HW Can only Support 8, 16, 32, 64 byte packets
            if((wPacketSize >= 8) && (wPacketSize < 16)){
                wPacketSize = 8;
            }
            else if ((wPacketSize >= 16) && (wPacketSize < 32)){
                wPacketSize = 16;
            }
            else if((wPacketSize >= 32) && (wPacketSize < 64)){
                wPacketSize = 32;
            }
            else if ((wPacketSize >= 64) && (wPacketSize < 128)){
                wPacketSize = 64;
            }
            else if((wPacketSize >= 128) && (wPacketSize < 256)){
                wPacketSize = 128;
            }
            else if ((wPacketSize >= 256) && (wPacketSize < 512)){
                wPacketSize = 256;
            }
            else if (wPacketSize >= 512){
                wPacketSize = 512; 
            }
            else{ // wPacketSize < 8
                dwRet = ERROR_INVALID_PARAMETER;
            }
            break;

        default:
            dwRet = ERROR_INVALID_PARAMETER;
            break;
        }  
        
        // If Requested Size is larger than what is supported ... change it.
        // Note only try and change it if no errors so far... meaning Ep is
        // Supportable.
        if ( (wPacketSize != (pEndpointDesc->wMaxPacketSize & USB_ENDPOINT_MAX_PACKET_SIZE_MASK)) && 
             (dwRet == ERROR_SUCCESS) ) {
            pEndpointDesc->wMaxPacketSize &= ~USB_ENDPOINT_MAX_PACKET_SIZE_MASK;
            pEndpointDesc->wMaxPacketSize |= wPacketSize;
        }

    }

    FUNCTION_LEAVE_MSG();

    return dwRet;
}


// Clear an endpoint stall.
DWORD
WINAPI
UfnPdd_ClearEndpointStall(
                          PVOID pvPddContext,
                          DWORD dwEndpoint
                          )
{
    DWORD dwRet = ERROR_SUCCESS;

    SETFNAME();
    FUNCTION_ENTER_MSG();


    DEBUGCHK(EP_VALID(dwEndpoint));

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    PEP_STATUS peps = GetEpStatus(pContext, dwEndpoint);
    LOCK_ENDPOINT(peps);


    RETAILMSG(DBG, (_T("%s  UfnPdd_ClearEndpointStall, EP : %d, \r\n"), pszFname,dwEndpoint));
 
    if (dwEndpoint == 0){
// Must Clear both Transmitted Stall and Stall Set
        SetClearReg(pContext, EP0SR, EP0SHT, SET);
        SetClearReg(pContext, EP0CR, EP0ESS, CLEAR);
   }
    else 
    { 
        // Must Clear both Send and Sent Stall
        SetClearIndexedReg(pContext, dwEndpoint, ECR, ESS, SET);
        SetClearIndexedReg(pContext, dwEndpoint, ECR, ESS, CLEAR);
        SetClearIndexedReg(pContext ,dwEndpoint, ESR, FSC, SET);
    }
    

	
    UNLOCK_ENDPOINT(peps);
    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}


// Initialize an endpoint.
DWORD
WINAPI
UfnPdd_InitEndpoint(
    PVOID                       pvPddContext,
    DWORD                       dwEndpoint,
    UFN_BUS_SPEED               Speed,
    PUSB_ENDPOINT_DESCRIPTOR    pEndpointDesc,
    PVOID                       pvReserved,
    BYTE                        bConfigurationValue,
    BYTE                        bInterfaceNumber,
    BYTE                        bAlternateSetting
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    DEBUGCHK(EP_VALID(dwEndpoint));
    PREFAST_DEBUGCHK(pEndpointDesc);

	RETAILMSG(DBG, (_T("%s  UfnPdd_InitEndpoint\r\n"),
            pszFname));

#ifdef DEBUG
    {
        USB_ENDPOINT_DESCRIPTOR EndpointDesc;
        memcpy(&EndpointDesc, pEndpointDesc, sizeof(EndpointDesc));
        DEBUGCHK(UfnPdd_IsEndpointSupportable(pvPddContext, dwEndpoint, Speed,
            &EndpointDesc, bConfigurationValue, bInterfaceNumber, 
            bAlternateSetting) == ERROR_SUCCESS);
        DEBUGCHK(memcmp(&EndpointDesc, pEndpointDesc, sizeof(EndpointDesc)) == 0);
    }
#endif

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);
    BYTE bEndpointAddress = 0;

    PEP_STATUS peps = GetEpStatus(pContext, dwEndpoint);
    DEBUGCHK(!peps->fInitialized);

    InitializeCriticalSection(&peps->cs);

    WORD wMaxPacketSize = 
        pEndpointDesc->wMaxPacketSize & USB_ENDPOINT_MAX_PACKET_SIZE_MASK;
    DEBUGCHK(wMaxPacketSize);
    
    // If the target is endpoint 0, then only allow the function driver 
    // to register a notification function.
    if (dwEndpoint == 0) {
       peps->dwPacketSizeAssigned = wMaxPacketSize;
       // Interrupts for endpoint 0 are enabled in ISTMain
    }
    else if (dwEndpoint < ENDPOINT_COUNT) {
        // Setup Direction (mode_in bit) 
        bEndpointAddress = pEndpointDesc->bEndpointAddress;
        BOOL fModeOut = USB_ENDPOINT_DIRECTION_OUT(bEndpointAddress);
        if (fModeOut) {
            SetClearReg(pContext, EDR, (0x1 << dwEndpoint), CLEAR);
	     peps->dwDirectionAssigned = USB_OUT_TRANSFER;
		
	     WriteIndexedReg(pContext, dwEndpoint, ECR, 0x0080);
	     WORD bECR = ReadIndexedReg(pContext, dwEndpoint, ECR);
		
        }
        else {
            SetClearReg(pContext, EDR, (0x1 << dwEndpoint), SET);
            peps->dwDirectionAssigned = USB_IN_TRANSFER;
	     WriteIndexedReg(pContext, dwEndpoint, ECR, 0x0000);
        }
        
        // Clear all Status bits and leave the Endpoint interrupt disabled
        // Clear Fifos, and all register bits
        ResetEndpoint(pContext,peps);

        // Set Transfer Type
        BYTE bTransferType = pEndpointDesc->bmAttributes & USB_ENDPOINT_TYPE_MASK;
        DEBUGCHK(bTransferType != USB_ENDPOINT_TYPE_CONTROL);
        switch(bTransferType) {
            case USB_ENDPOINT_TYPE_ISOCHRONOUS:
                // Set the ISO bit
                SetClearIndexedReg(pContext, dwEndpoint,ECR, 
                    IME, SET);
                break;

            case USB_ENDPOINT_TYPE_BULK:
            case USB_ENDPOINT_TYPE_INTERRUPT:
            default:
                // Clear ISO bit - Set type to Bulk
                SetClearIndexedReg(pContext, dwEndpoint,ECR, 
                    IME, CLEAR);
        }

        peps->dwEndpointType = bTransferType;
        peps->dwPacketSizeAssigned = wMaxPacketSize;
        
        // Set the Max Packet Register
        WriteIndexedReg(pContext, dwEndpoint, MPR, wMaxPacketSize); 

        UfnPdd_ClearEndpointStall(pvPddContext,dwEndpoint);

        // Clear outstanding interrupts
        ClearEndpointInterrupt(pContext, dwEndpoint);

    }

	//RETAILMSG(1, (_T(" UfnPdd_InitEndpoint: %d,wMaxPacketSize: %d "),dwEndpoint,wMaxPacketSize ));
	
    peps->fInitialized = TRUE;
    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}


// Deinitialize an endpoint.
DWORD
WINAPI
UfnPdd_DeinitEndpoint(
                      PVOID pvPddContext,
                      DWORD dwEndpoint
                      )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    DEBUGCHK(EP_VALID(dwEndpoint));

	RETAILMSG(DBG, (_T("%s  UfnPdd_DeinitEndpoint\r\n"),
            pszFname));

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    PEP_STATUS peps = GetEpStatus(pContext, dwEndpoint);
    LOCK_ENDPOINT(peps);

    DEBUGCHK(peps->fInitialized);
    DEBUGCHK(peps->pTransfer == NULL);

    // Reset and disable the endpoint 
    // Mask endpoint interrupts    
    ResetEndpoint(pContext, peps);

    // Clear endpoint interrupts
    ClearEndpointInterrupt(pContext, dwEndpoint);


    peps->fInitialized = FALSE;
    UNLOCK_ENDPOINT(peps);

    DeleteCriticalSection(&peps->cs);

    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}

DWORD
WINAPI
UfnPdd_StallEndpoint(
                     PVOID pvPddContext,
                     DWORD dwEndpoint
                     )
{
    DWORD dwRet = ERROR_SUCCESS;
//    WORD bEP0CR, bECR;
    SETFNAME();
    FUNCTION_ENTER_MSG();

    DEBUGCHK(EP_VALID(dwEndpoint));

  
    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);


    RETAILMSG(DBG, (_T("%s  UfnPdd_StallEndpoint, EP : %d, \r\n"), pszFname,dwEndpoint));


    PEP_STATUS peps = GetEpStatus(pContext, dwEndpoint);
    DEBUGCHK(peps->fInitialized);
    LOCK_ENDPOINT(peps);

    if (dwEndpoint == 0) {
        // Must Clear Out Packet Ready when sending Stall
//        DEBUGMSG(ZONE_SEND, (_T("%s Writing 0x%02x to EP0_CSR_REG\r\n"), pszFname,
//		bEp0StallBits));


	 SetClearIndexedReg(pContext, dwEndpoint, EP0CR, EP0ESS, SET);
		 
        // Set Flag so that SendControlStatusHandshked does not
        // duplicate this HW Write. Manual says all bits need
        // to be set at the same time.
        pContext->sendDataEnd = FALSE;
        pContext->Ep0State = EP0_STATE_IDLE;
    }
    else { 
       // Must Clear Out Packet Ready when sending Stall
	   
           SetClearIndexedReg(pContext, dwEndpoint, ECR, ESS, SET);

    }
	
    UNLOCK_ENDPOINT(peps);
    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}


// Send the control status handshake.
DWORD
WINAPI
UfnPdd_SendControlStatusHandshake(
    PVOID           pvPddContext,
    DWORD           dwEndpoint
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

	RETAILMSG(DBG, (_T("%s  UfnPdd_SendControlStatusHandshake\r\n"),
            pszFname));

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    DEBUGCHK(dwEndpoint == 0);
    
    // This function is only valid for Endpoint 0
    EP_STATUS *peps = GetEpStatus(pContext, 0);
    DEBUGCHK(peps->fInitialized);

    // Remove the Out Packet Ready Condition
    if(pContext->sendDataEnd) {
		LOCK_ENDPOINT(peps);

        DEBUGMSG(ZONE_SEND, (_T("%s Sending 0 packet \r\n"), pszFname));
        
        pContext->sendDataEnd = FALSE;

		UNLOCK_ENDPOINT(peps);
    }
	
    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}


// Set the address of the device on the USB.
DWORD
WINAPI
UfnPdd_SetAddress(
                  PVOID pvPddContext,
                  BYTE  bAddress
                  )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

	RETAILMSG(DBG, (_T("%s  UfnPdd_SetAddress\r\n"),
            pszFname));

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

   
    WriteReg(pContext, FARR, bAddress);
	
    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}


// Is endpoint Stalled?
DWORD
WINAPI
UfnPdd_IsEndpointHalted(
    PVOID pvPddContext,
    DWORD dwEndpoint,
    PBOOL pfHalted
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

    DWORD dwRet = ERROR_SUCCESS;
    WORD bRegVal;
    BOOL fHalted = FALSE;

	RETAILMSG(DBG, (_T("%s  UfnPdd_IsEndpointHalted: %d\r\n"),
            pszFname,dwEndpoint));

    DEBUGCHK(EP_VALID(dwEndpoint));
    PREFAST_DEBUGCHK(pfHalted);

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    PEP_STATUS peps = GetEpStatus(pContext, dwEndpoint);
    LOCK_ENDPOINT(peps);

    // Check the Appropriate Stall Bit
    if (dwEndpoint == 0) {
        // Must Clear Out Packet Ready when sending Stall
        bRegVal = ReadReg(pContext, EP0CR);
        if (bRegVal & EP0ESS)
            fHalted = TRUE;            
    }
    else 
    { 
        bRegVal = ReadIndexedReg(pContext,dwEndpoint, ECR);
        if (bRegVal & ESS)
            fHalted = TRUE;
    }

    *pfHalted = fHalted;
    pContext->sendDataEnd  = FALSE;
    UNLOCK_ENDPOINT(peps);

    FUNCTION_LEAVE_MSG();

    return dwRet;
}


DWORD
WINAPI
UfnPdd_InitiateRemoteWakeup(
    PVOID pvPddContext
    )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

	RETAILMSG(DBG, (_T("%s  UfnPdd_InitiateRemoteWakeup\r\n"),
            pszFname));

    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    SetClearReg(pContext, SCR, (RRDE | MFRM), SET);

    FUNCTION_LEAVE_MSG();

    return ERROR_SUCCESS;
}


DWORD
WINAPI
UfnPdd_RegisterDevice(
    PVOID                           pvPddContext,
    PCUSB_DEVICE_DESCRIPTOR         pHighSpeedDeviceDesc,
    PCUFN_CONFIGURATION             pHighSpeedConfig,
    PCUSB_CONFIGURATION_DESCRIPTOR  pHighSpeedConfigDesc,
    PCUSB_DEVICE_DESCRIPTOR         pFullSpeedDeviceDesc,
    PCUFN_CONFIGURATION             pFullSpeedConfig,
    PCUSB_CONFIGURATION_DESCRIPTOR  pFullSpeedConfigDesc,
    PCUFN_STRING_SET                pStringSets,
    DWORD                           cStringSets
    )
{
    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);
    
    // Nothing to do.

    return ERROR_SUCCESS;
}


DWORD
WINAPI
UfnPdd_DeregisterDevice(
    PVOID   pvPddContext
    )
{
    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    return ERROR_SUCCESS;
}


VOID
WINAPI
UfnPdd_PowerDown(
    PVOID pvPddContext
    )
{
    SETFNAME();
    DEBUGMSG(ZONE_POWER, (_T("%s\r\n"), pszFname));
    
    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    // Nothing to do.
}


VOID
WINAPI
UfnPdd_PowerUp(
    PVOID pvPddContext
    )
{
    SETFNAME();
    DEBUGMSG(ZONE_POWER, (_T("%s\r\n"), pszFname));
    
    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    // Nothing to do.
}


DWORD
WINAPI
UfnPdd_IOControl(
                 PVOID           pvPddContext,
                 IOCTL_SOURCE    source,
                 DWORD           dwCode,
                 PBYTE           pbIn,
                 DWORD           cbIn,
                 PBYTE           pbOut,
                 DWORD           cbOut,
                 PDWORD          pcbActualOut
                 )
{
    SETFNAME();
    FUNCTION_ENTER_MSG();

	RETAILMSG(DBG, (_T("%s  UfnPdd_IOControl\r\n"),
            pszFname));


    PCTRLR_PDD_CONTEXT pContext = (PCTRLR_PDD_CONTEXT) pvPddContext;
    ValidateContext(pContext);

    DWORD dwRet = ERROR_INVALID_PARAMETER;

    switch (dwCode) {
    case IOCTL_UFN_GET_PDD_INFO:
        if ( source != BUS_IOCTL || pbOut == NULL || 
             cbOut != sizeof(UFN_PDD_INFO) ) {
            break;
        }

        // Not currently supported.
        break;

    case IOCTL_BUS_GET_POWER_STATE:
        if (source == MDD_IOCTL) {
            PREFAST_DEBUGCHK(pbIn);
            DEBUGCHK(cbIn == sizeof(CE_BUS_POWER_STATE));

            PCE_BUS_POWER_STATE pCePowerState = (PCE_BUS_POWER_STATE) pbIn;
            PREFAST_DEBUGCHK(pCePowerState->lpceDevicePowerState);

            DEBUGMSG(ZONE_POWER, (_T("%s IOCTL_BUS_GET_POWER_STATE\r\n"), pszFname));

            *pCePowerState->lpceDevicePowerState = pContext->cpsCurrent;

            dwRet = ERROR_SUCCESS;
        }
        break;

    case IOCTL_BUS_SET_POWER_STATE:
        if (source == MDD_IOCTL) {
            PREFAST_DEBUGCHK(pbIn);
            DEBUGCHK(cbIn == sizeof(CE_BUS_POWER_STATE));

            PCE_BUS_POWER_STATE pCePowerState = (PCE_BUS_POWER_STATE) pbIn;

            PREFAST_DEBUGCHK(pCePowerState->lpceDevicePowerState);
            DEBUGCHK(VALID_DX(*pCePowerState->lpceDevicePowerState));

            DEBUGMSG(ZONE_POWER, (_T("%s IOCTL_BUS_GET_POWER_STATE(D%u)\r\n"), 
                pszFname, *pCePowerState->lpceDevicePowerState));

            SetPowerState(pContext, *pCePowerState->lpceDevicePowerState);

            dwRet = ERROR_SUCCESS;
        }
        break;
    }

    FUNCTION_LEAVE_MSG();

    return dwRet;
}



// Initialize the device.
DWORD
WINAPI
UfnPdd_Init(
    LPCTSTR                     pszActiveKey,
    PVOID                       pvMddContext,
    PUFN_MDD_INTERFACE_INFO     pMddInterfaceInfo,
    PUFN_PDD_INTERFACE_INFO     pPddInterfaceInfo
    )
{    
    SETFNAME();
    FUNCTION_ENTER_MSG();

    static const UFN_PDD_INTERFACE_INFO sc_PddInterfaceInfo = {
        UFN_PDD_INTERFACE_VERSION,
        (UFN_PDD_CAPS_SUPPORTS_FULL_SPEED | UFN_PDD_CAPS_SUPPORTS_HIGH_SPEED),
        ENDPOINT_COUNT,
        NULL, // This gets filled in later
        
        &UfnPdd_Deinit,
        &UfnPdd_IsConfigurationSupportable,
        &UfnPdd_IsEndpointSupportable,
        &UfnPdd_InitEndpoint,
        &UfnPdd_RegisterDevice,
        &UfnPdd_DeregisterDevice,
        &UfnPdd_Start,
        &UfnPdd_Stop,
        &UfnPdd_IssueTransfer,
        &UfnPdd_AbortTransfer,
        &UfnPdd_DeinitEndpoint,
        &UfnPdd_StallEndpoint,
        &UfnPdd_ClearEndpointStall,
        &UfnPdd_SendControlStatusHandshake,
        &UfnPdd_SetAddress,
        &UfnPdd_IsEndpointHalted,
        &UfnPdd_InitiateRemoteWakeup,
        &UfnPdd_PowerDown,
        &UfnPdd_PowerUp,
        &UfnPdd_IOControl,
    };

    DWORD dwType;
    DWORD dwRet;

    HKEY hkDevice = NULL;
    HKEY hKey = NULL;
    PCTRLR_PDD_CONTEXT pContext = NULL;

    DEBUGCHK(pszActiveKey);
    DEBUGCHK(pMddInterfaceInfo);
    DEBUGCHK(pPddInterfaceInfo);

    RETAILMSG(DBG, (_T(" USB Function Driver Init \r\n")));

    hkDevice = OpenDeviceKey(pszActiveKey);
    if (!hkDevice) {
        dwRet = GetLastError();
        RETAILMSG(DBG, (_T("%s Could not open device key. Error: %d\r\n"), 
            pszFname, dwRet));
        goto EXIT;        
    }
    
    // woo
    DWORD dwTmp;
    WCHAR Data[20];
    DWORD cbData;
    WCHAR *pData=Data;	
    cbData = sizeof( Data );

    dwTmp = RegOpenKeyEx( HKEY_LOCAL_MACHINE, DRIVER_USB_KEY, 0, 0, &hKey );
    dwType = REG_SZ;
    USBClassInfo = USB_RNDIS; // RNDIS is default class
            	
    if( dwTmp == ERROR_SUCCESS )
    {        
        dwTmp = RegQueryValueEx( hKey, DRIVER_USB_VALUE, NULL, &dwType, (LPBYTE)Data, &cbData );
        RETAILMSG(DBG, (_T("RegOpenKeyEx success %d, %d, %s, %d \r\n"),
        	dwTmp, dwType, Data, cbData ));
        if( dwTmp == ERROR_SUCCESS && dwType == REG_SZ )
        {

            pData =_wcsupr(Data);
             RETAILMSG(1, (_T("pData=%s\r\n"), pData));
		
            RETAILMSG(DBG, (_T("RegQueryValueEx success \r\n")));
//            if( wcscmp(Data, TEXT("RNDIS")) == 0 )
            if( wcscmp(pData, TEXT("RNDIS")) == 0 )
            {
                USBClassInfo = USB_RNDIS;
                RETAILMSG(1, (_T("USB RNDIS Function Class Enabled : %s \r\n"), Data));
            }
//            else if( wcscmp(Data, TEXT("Serial_Class")) == 0 )
            else if( wcscmp(pData, TEXT("SERIAL_CLASS")) == 0 )
            {
            	USBClassInfo = USB_Serial;
                RETAILMSG(1, (_T("USB Serial Function Class Enabled : %s \r\n"), Data));
            }
//            else if( wcscmp(Data, TEXT("Mass_Storage_Class")) == 0 )
            else if( wcscmp(pData, TEXT("MASS_STORAGE_CLASS")) == 0 )
            {
            	USBClassInfo = USB_MSF;
                RETAILMSG(1, (_T("USB MSF Function Class Enabled : %s \r\n"), Data));
			
	if(bSDMMCMSF) {
		RETAILMSG(1, (_T("SDMMCMSF TRUE \r\n")));
		Sleep(1000);
		}
            }
        }
        
        RegCloseKey( hKey );
    }
    
	DPNOK(bSDMMCMSF);
	
    pContext = (PCTRLR_PDD_CONTEXT) LocalAlloc(LPTR, sizeof(*pContext));
    if (pContext == NULL) {
        dwRet = GetLastError();
        PREFAST_DEBUGCHK(dwRet != ERROR_SUCCESS);
        RETAILMSG(DBG, (_T("%s LocalAlloc failed. Error: %d\r\n"), pszFname, dwRet));
        goto EXIT;
    }
    pContext->dwSig = SC2450_SIG;

    pContext->pvMddContext = pvMddContext;
    pContext->cpsCurrent = (CEDEVICE_POWER_STATE)D4;
    pContext->dwIrq = IRQ_UNSPECIFIED;
    pContext->pfnNotify = pMddInterfaceInfo->pfnNotify;
    InitializeCriticalSection(&pContext->csIndexedRegisterAccess);

    for (DWORD dwEp = 0; dwEp < dim(pContext->rgEpStatus); ++dwEp) {
        pContext->rgEpStatus[dwEp].dwEndpointNumber = dwEp;
    }

    DWORD dwDataSize;
    DWORD dwPriority;

    DDKISRINFO dii;
    DDKWINDOWINFO dwi;

    // read window configuration from the registry
    dwi.cbSize = sizeof(dwi);
    dwRet = DDKReg_GetWindowInfo(hkDevice, &dwi);
    if(dwRet != ERROR_SUCCESS) {
        RETAILMSG(DBG, (_T("%s DDKReg_GetWindowInfo() failed %d\r\n"), 
            pszFname, dwRet));
        goto EXIT;
    } 
    else if (dwi.dwNumIoWindows != 1) {
        RETAILMSG(DBG, (_T("%s %d windows configured, expected 1\r\n"), 
            pszFname, dwi.dwNumIoWindows));
        dwRet = ERROR_INVALID_DATA;
        goto EXIT;
    } 
    else if (dwi.ioWindows[0].dwLen < REGISTER_SET_SIZE) {
        RETAILMSG(DBG, (_T("%s ioLen of 0x%x is less than required 0x%x\r\n"),
            pszFname, dwi.ioWindows[0].dwLen, REGISTER_SET_SIZE));
        dwRet = ERROR_INVALID_DATA;
        goto EXIT;        
    }
    else if (dwi.ioWindows[0].dwBase == 0){
        RETAILMSG(DBG, (_T("%s no ioBase value specified\r\n"),pszFname));
        dwRet = ERROR_INVALID_DATA;
        goto EXIT;
    }
    else {
        pContext->dwIOBase = dwi.ioWindows[0].dwBase;
        pContext->dwIOLen = dwi.ioWindows[0].dwLen;
    }
    
    // get ISR configuration information
    dii.cbSize = sizeof(dii);
    dwRet = DDKReg_GetIsrInfo(hkDevice, &dii);
    if (dwRet != ERROR_SUCCESS) {
        RETAILMSG(DBG, (_T("%s DDKReg_GetIsrInfo() failed %d\r\n"), 
            pszFname, dwRet));
        goto EXIT;
    }
    else if( (dii.dwSysintr == SYSINTR_NOP) && (dii.dwIrq == IRQ_UNSPECIFIED) ) {
        RETAILMSG(DBG, (_T("%s no IRQ or SYSINTR value specified\r\n"), pszFname));
        dwRet = ERROR_INVALID_DATA;
        goto EXIT;
    } 
    else {
        if (dii.dwSysintr == SYSINTR_NOP) {
            BOOL fSuccess = KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, &dii.dwIrq, 
                sizeof(DWORD), &dii.dwSysintr, sizeof(DWORD), NULL);
            if (!fSuccess) {
                RETAILMSG(DBG, (_T("%s IOCTL_HAL_REQUEST_SYSINTR failed!\r\n"), 
                    pszFname));
                goto EXIT;
            }

            pContext->dwIrq = dii.dwIrq;
            pContext->dwSysIntr = dii.dwSysintr;
        }
        else {
            pContext->dwSysIntr = dii.dwSysintr;
        }
    }

    // Read the IST priority
    dwDataSize = sizeof(dwPriority);
    dwRet = RegQueryValueEx(hkDevice, UDC_REG_PRIORITY_VAL, NULL, &dwType,
        (LPBYTE) &dwPriority, &dwDataSize);
    if (dwRet != ERROR_SUCCESS) {
        dwPriority = DEFAULT_PRIORITY;
    }


    pContext->hBusAccess = CreateBusAccessHandle(pszActiveKey);
    if (pContext->hBusAccess == NULL) {
        // This is not a failure.
        DEBUGMSG(ZONE_WARNING, (_T("%s Could not create bus access handle\r\n"),
            pszFname));
    }

    RETAILMSG(DBG, (_T("%s Using IO Base %x\r\n"), 
        pszFname, pContext->dwIOBase));
    RETAILMSG(DBG, (_T("%s Using SysIntr %u\r\n"), 
        pszFname, pContext->dwSysIntr));
    RETAILMSG(DBG, (_T("%s Using IST priority %u\r\n"), 
        pszFname, dwPriority));

    pContext->dwISTPriority = dwPriority;

    // map register space to virtual memory
    dwRet = MapRegisterSet(pContext);
    if (dwRet != ERROR_SUCCESS) {
        RETAILMSG(DBG, (_T("%s failed to map register space\r\n"),
            pszFname));
        goto EXIT;
    }


    pContext->attachedState = UFN_DETACH;
    pContext->dwResetCount = 0;

    ResetDevice(pContext);
    ValidateContext(pContext);

    memcpy(pPddInterfaceInfo, &sc_PddInterfaceInfo, sizeof(sc_PddInterfaceInfo));
    pPddInterfaceInfo->pvPddContext = pContext;
    
    // EINT2 Test Code by woo

    dwRet = HW_InitRegisters();
    if ( dwRet ) {
	    RETAILMSG(1, (TEXT("HW_Init : HW_InitRegisters  Error (0x%x) \r\n"), dwRet));
        goto EXIT;
    }
// end woo

    g_PlugEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, &g_PlugIrq, sizeof(UINT32), &g_PlugSysIntr, sizeof(UINT32), NULL))
    {
        RETAILMSG(1, (TEXT("ERROR: Plug Failed to request sysintr value for Plug interrupt.\r\n")));
        return(0);
    }  
    RETAILMSG(1,(TEXT("INFO: Plug: Mapped Irq 0x%x to SysIntr 0x%x.\r\n"), g_PlugIrq, g_PlugSysIntr));
	if (!(InterruptInitialize(g_PlugSysIntr, g_PlugEvent, 0, 0))) 
	{
		RETAILMSG(1, (TEXT("ERROR: Plug: Interrupt initialize failed.\r\n")));
	}
    if ( (g_PlugThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) PLUG_IST, pContext, 0, NULL)) == NULL) {
        dwRet = GetLastError();
        RETAILMSG(1,(TEXT("PCF50606 ERROR: Unable to create IST: %u\r\n"), dwRet));
        goto EXIT;
    }	
    if ( !CeSetThreadPriority(g_PlugThread, POWER_THREAD_PRIORITY)) {
        dwRet = GetLastError();
        RETAILMSG(1, (TEXT("PCF50606 ERROR: CeSetThreadPriority ERROR:%d \r\n"), dwRet));
        goto EXIT;
    }    
   if(bSDMMCMSF && (USBClassInfo == USB_MSF))
   	{
    if ( (g_SdCardDetectThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) SDCARD_DetectIST, pContext, 0, NULL)) == NULL) {
        dwRet = GetLastError();
        RETAILMSG(1,(TEXT("PCF50606 ERROR: Unable to create IST: %u\r\n"), dwRet));
        goto EXIT;
    }	
    if ( !CeSetThreadPriority(g_SdCardDetectThread, POWER_THREAD_PRIORITY)) {
        dwRet = GetLastError();
        RETAILMSG(1, (TEXT("PCF50606 ERROR: CeSetThreadPriority ERROR:%d \r\n"), dwRet));
        goto EXIT;
	    }  
    }  

 //[david.modify] 2008-08-19 02:10
 //创建一个线程，用来解决第1次发送给AP事件，AP收不着的问题
 //======================
   if(bSDMMCMSF && (USBClassInfo == USB_MSF))
   	{
    if ( (g_tmpThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) SendEvent2App_IST, pContext, 0, NULL)) == NULL) {
        dwRet = GetLastError();
	DPN(dwRet);
        goto EXIT;
    }	
    if ( !CeSetThreadPriority(g_tmpThread, POWER_THREAD_PRIORITY)) {
        dwRet = GetLastError();
	DPN(dwRet);
        goto EXIT;
	    }  
    }  

 //======================   
   
EXIT:
    if (hkDevice) RegCloseKey(hkDevice);
    
    if (dwRet != ERROR_SUCCESS && pContext) {
        FreeCtrlrContext(pContext);
    }

    FUNCTION_LEAVE_MSG();

    return dwRet;
}


// Called by MDD's DllEntry.
extern "C" 
BOOL
UfnPdd_DllEntry(
    HANDLE hDllHandle,
    DWORD  dwReason, 
    LPVOID lpReserved
    )
{
    SETFNAME();

    switch (dwReason) {
        case DLL_PROCESS_ATTACH:
            g_pUDCBase = NULL;
            break;
    }

    return TRUE;
}

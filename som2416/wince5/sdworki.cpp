//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
//
// Use of this source code is subject to the terms of the Microsoft end-user
// license agreement (EULA) under which you licensed this SOFTWARE PRODUCT.
// If you did not accept the terms of the EULA, you are not authorized to use
// this source code. For a copy of the EULA, please see the LICENSE.RTF on your
// install media.
//
//
/*++
THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
PARTICULAR PURPOSE.

Module Name:  
    SDWorki.cpp
Abstract:
    SD Async Work Item  Implementation.

Notes: 
--*/
#include <windows.h>
#include <types.h>
#include "../SDDriver_INC/sdhcd.h"

#include "sdbus.hpp"
#include "sdslot.hpp"
#include "sdworki.hpp"

///////////////////////////////////////////////////////////////////////////////
//  CSDWorkItem - constructor 
//  Input: 
//  Output: 
//  Notes:  
//      
///////////////////////////////////////////////////////////////////////////////
CSDWorkItem::CSDWorkItem(DWORD dwMaxItem)
:   CMiniThread( 0, TRUE)
,   m_dwMaxOfSlotEvent(max(dwMaxItem,MIN_WORK_ITEM))
{
    m_psdSlotEvent = new SD_SLOT_EVENT [m_dwMaxOfSlotEvent];
    m_hWakeupEvent = CreateEvent(NULL,FALSE,TRUE,NULL);
    m_hEmptySlotSem = CreateSemaphore(NULL, m_dwMaxOfSlotEvent-1, m_dwMaxOfSlotEvent-1,NULL);
    m_dwReadIndex = m_dwWriteIndex = 0; 
}

///////////////////////////////////////////////////////////////////////////////
//  ~CSDWorkItem - destructor
//  Input:  
//  Output: 
//  Notes:  
//      
///////////////////////////////////////////////////////////////////////////////
CSDWorkItem::~CSDWorkItem() 
{

    m_bTerminated = TRUE;
    ThreadStart();
    if ( m_hWakeupEvent )
        SetEvent(m_hWakeupEvent);
    BOOL fRet = WaitThreadComplete( 5000 );
    ASSERT(fRet);
    if (m_hEmptySlotSem!=NULL)
        CloseHandle(m_hEmptySlotSem);
    if (m_hWakeupEvent!=NULL)
        CloseHandle(m_hWakeupEvent);
    delete m_psdSlotEvent;
}

BOOL CSDWorkItem::Init(DWORD  dwCeThreadPriority)
{
    if (m_psdSlotEvent!=NULL && m_hWakeupEvent!=NULL && m_hEmptySlotSem!=NULL) {
         CeSetPriority( (int)dwCeThreadPriority );
         ThreadStart();
         return TRUE;
    };
    ASSERT(FALSE);
    return FALSE;
    
}
///////////////////////////////////////////////////////////////////////////////
//  PostMessage - post a message
//  Input:  pMessage - message to post
//  Output: 
//  Return:
//  Notes:  
//      
///////////////////////////////////////////////////////////////////////////////
BOOL CSDWorkItem::PostEvent(SD_SLOT_EVENT sdEvent, DWORD dwWaitTick)
{
    PREFAST_ASSERT(m_psdSlotEvent!=NULL);
    BOOL fRet = FALSE;
    //if (::WaitForSingleObject( m_hEmptySlotSem, dwWaitTick ) == WAIT_OBJECT_0 ) {
        Lock();
        if (!IsFull()) {
            m_psdSlotEvent[m_dwWriteIndex] = sdEvent;
            m_dwWriteIndex = IncIndex(m_dwWriteIndex);
            SetEvent(m_hWakeupEvent);
            fRet = TRUE;
        }
        else
            ASSERT(FALSE);
        Unlock();
    //}
    return fRet;
}

DWORD  CSDWorkItem::ThreadRun()
{
    while (!m_bTerminated) {
        PREFAST_ASSERT(m_psdSlotEvent!=NULL);
        if (!IsEmpty()) {
            // We don't need take lock since only this thread modify the Read Index.
            SD_SLOT_EVENT sdEvent = m_psdSlotEvent [m_dwReadIndex];
            m_dwReadIndex = IncIndex(m_dwReadIndex);
            BOOL fRet = ReleaseSemaphore(m_hEmptySlotSem,1,NULL);
            ASSERT(fRet);
            SlotStatusChangeProcessing(sdEvent);
        }
        else
            ::WaitForSingleObject( m_hWakeupEvent, INFINITE );
    }
    return FALSE;
}



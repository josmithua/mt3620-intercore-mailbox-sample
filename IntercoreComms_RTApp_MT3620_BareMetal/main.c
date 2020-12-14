/* Copyright (c) Microsoft Corporation. All rights reserved.
   Copyright (c) Codethink Ltd. All rights reserved.
   Licensed under the MIT License. */

// This sample C application for the real-time core demonstrates intercore communications by
// sending a message to a high-level application every second, and printing out any received
// messages.
//
// It demontrates the following hardware
// - UART (used to write a message via the built-in UART)
// - mailbox (used to report buffer sizes and send / receive events)
// - timer (used to send a message to the HLApp)

/* Copyright (c) Codethink Ltd. All rights reserved.
   Licensed under the MIT License. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/CPUFreq.h"
#include "lib/GPIO.h"
#include "lib/GPT.h"
#include "lib/NVIC.h"
#include "lib/Print.h"
#include "lib/UART.h"
#include "lib/VectorTable.h"
#include "lib/mt3620/gpt.h"

#include "Socket.h"

// Drivers
static UART *debug = NULL;
static GPT *sendTimer = NULL;

static Socket *socket = NULL;

static volatile unsigned msgCounter = 0;

// Callbacks
typedef struct CallbackNode {
    bool enqueued;
    struct CallbackNode *next;
    void *data;
    void (*cb)(void *);
} CallbackNode;

static void EnqueueCallback(CallbackNode *node);

// Msg callbacks
// Prints an array of bytes
static void printBytes(const uint8_t *bytes, uintptr_t start, uintptr_t size) {
    for (unsigned i = start; i < size; i++) {
        UART_Printf(debug, "%x", bytes[i]);
    }
}

static void printComponentId(const Component_Id *compId) {
    UART_Printf(debug, "%lx-%x-%x", compId->seg_0, compId->seg_1, compId->seg_2);
    UART_Print(debug, "-");
    printBytes(compId->seg_3_4, 0, 2);
    UART_Print(debug, "-");
    printBytes(compId->seg_3_4, 2, 8);
    UART_Print(debug, "\r\n");
}

static void handleSendMsgTimer(void *data) {
    static const Component_Id A7ID = {
        .seg_0 = 0x25025d2c,
        .seg_1 = 0x66da,
        .seg_2 = 0x4448,
        .seg_3_4 = {0xba, 0xe1, 0xac, 0x26, 0xfc, 0xdd, 0x36, 0x27},
    };

    static char msg[] = "rt-app-to-hl-app-00";
    const uintptr_t msgLen = sizeof(msg);

    msg[msgLen - 2] = '0' + (msgCounter % 10);
    msg[msgLen - 3] = '0' + (msgCounter / 10);
    msgCounter = (msgCounter + 1) % 100;

    int32_t error = Socket_Write(socket, &A7ID, msg, msgLen);

    if (error != ERROR_NONE) {
        UART_Printf(debug, "ERROR: sending msg %s - %ld\r\n", msg, error);
    }
}

static void handleSendMsgTimerWrapper(GPT *timer) {
    (void)(timer);

    static CallbackNode cbn = {.enqueued = false, .cb = handleSendMsgTimer, .data = NULL};
    EnqueueCallback(&cbn);
}

static void handleRecvMsg(void *handle) {
    Socket *socket = (Socket *)handle;

    Component_Id senderId;
    static char msg[32];
    uint32_t msg_size = sizeof(msg);

    int32_t error = Socket_Read(socket, &senderId, msg, &msg_size);

    if (error != ERROR_NONE) {
        UART_Printf(debug, "ERROR: receiving msg %s - %ld\r\n", msg, error);
    }

    msg[msg_size] = '\0';
    UART_Printf(debug, "Message received: %s\r\nSender: ", msg);
    printComponentId(&senderId);
}

static void handleRecvMsgWrapper(Socket *handle) {
    static CallbackNode cbn = {.enqueued = false, .cb = handleRecvMsg, .data = NULL};

    if (!cbn.data) {
        cbn.data = handle;
    }
    EnqueueCallback(&cbn);
}

static CallbackNode *volatile callbacks = NULL;

static void EnqueueCallback(CallbackNode *node) {
    uint32_t prevBasePri = NVIC_BlockIRQs();
    if (!node->enqueued) {
        CallbackNode *prevHead = callbacks;
        node->enqueued = true;
        callbacks = node;
        node->next = prevHead;
    }
    NVIC_RestoreIRQs(prevBasePri);
}

static void InvokeCallbacks(void) {
    CallbackNode *node;
    do {
        uint32_t prevBasePri = NVIC_BlockIRQs();
        node = callbacks;
        if (node) {
            node->enqueued = false;
            callbacks = node->next;
        }
        NVIC_RestoreIRQs(prevBasePri);

        if (node) {
            (node->cb)(node->data);
        }
    } while (node);
}

_Noreturn void RTCoreMain(void) {
    VectorTableInit();
    CPUFreq_Set(197600000);

    debug = UART_Open(MT3620_UNIT_UART_DEBUG, 115200, UART_PARITY_NONE, 1, NULL);
    UART_Print(debug, "--------------------------------\r\n");
    UART_Print(debug, "IntercoreComms_MT3620_BareMetal\r\n");
    UART_Print(debug, "App built on: " __DATE__ " " __TIME__ "\r\n");

    // Initialise timer
    sendTimer = GPT_Open(MT3620_UNIT_GPT3, MT3620_GPT_3_SRC_CLK_HZ, GPT_MODE_REPEAT);
    if (!sendTimer) {
        UART_Printf(debug, "ERROR: GPT3 initialisation failed\r\n");
    }

    // Setup socket
    socket = Socket_Open(handleRecvMsgWrapper);
    if (!socket) {
        UART_Printf(debug, "ERROR: socket initialisation failed\r\n");
    }

    // Setup Msg out
    int32_t error;
    if ((error = GPT_StartTimeout(sendTimer, 5, GPT_UNITS_MICROSEC, handleSendMsgTimerWrapper)) != ERROR_NONE) {
        UART_Printf(debug, "ERROR: Msg GPT_StartTimeout failed %ld\r\n", error);
    }

    for (;;) {
        __asm__("wfi");
        InvokeCallbacks();
    }
}

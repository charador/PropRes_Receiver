/**************************************************************************
 *
 * Copyright (C) 2005 Steve Karg <skarg@users.sourceforge.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *********************************************************************/
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>//for rand())
#include <time.h>// rand())
#include <assert.h>
#include "config.h"
#include "txbuf.h"
#include "bacdef.h"
#include "bacdcode.h"
#include "bacerror.h"
#include "apdu.h"
#include "npdu.h"

#include "abort.h"
#include "wp.h"
/* device object has the handling for all objects */
#include "device.h"
#include "handlers.h"
#include "client.h"
#include "av.h"//for execute WP commands

#define _GNU_SOURCE

#include <unistd.h>
#include <sys/syscall.h>
#include <linux/random.h>
/** @file h_wp.c  Handles Write Property requests. */

uint8_t global_random[8] = {0};
extern BACNET_WRITE_PROPERTY_DATA global_wp_data;

/* generates random number stream*/
void gen_rdm_bytestream(size_t num_bytes, uint8_t * random) {
    size_t i;

    for (i = 0; i < num_bytes; i++) {
        random[i] = rand();
    }
    return;
}

/** Handler for a WriteProperty Service request.
 * @ingroup DSWP
 * This handler will be invoked by apdu_handler() if it has been enabled
 * by a call to apdu_set_confirmed_handler().
 * This handler builds a response packet, which is
 * - an Abort if
 *   - the message is segmented
 *   - if decoding fails
 * - an ACK if Device_Write_Property() succeeds
 * - an Error if Device_Write_Property() fails
 *   or there isn't enough room in the APDU to fit the data.
 *
 * @param service_request [in] The contents of the service request.
 * @param service_len [in] The length of the service_request.
 * @param src [in] BACNET_ADDRESS of the source of the message
 * @param service_data [in] The BACNET_CONFIRMED_SERVICE_DATA information
 *                          decoded from the APDU header of this message.
 */
void handler_write_property(
        uint8_t * service_request,
        uint16_t service_len,
        BACNET_ADDRESS * src,
        BACNET_CONFIRMED_SERVICE_DATA * service_data) {
    BACNET_WRITE_PROPERTY_DATA wp_data;
    int len = 0;
    int pdu_len = 0;
    BACNET_NPDU_DATA npdu_data;
    int bytes_sent = 0;
    BACNET_ADDRESS my_address;
    int val= -1;
    uint8_t random[8] = {0};
    int remain = 0;
    int tmp_pdu_len = 0;

    /*another way to gen random number by sys call*/
    void *buf = NULL;
    size_t l = 5;
    unsigned int o = 1;
    int r = syscall(SYS_getrandom, buf, l, o);

    /* encode the NPDU portion of the packet */
    datalink_get_my_address(&my_address);
    npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);
    pdu_len =
            npdu_encode_pdu(&Handler_Transmit_Buffer[0], src, &my_address,
            &npdu_data);
#if PRINT_ENABLED
    fprintf(stderr, "WP: Received Request!\n");
#endif
    if (service_data->segmented_message) {
        len =
                abort_encode_apdu(&Handler_Transmit_Buffer[pdu_len],
                service_data->invoke_id, ABORT_REASON_SEGMENTATION_NOT_SUPPORTED,
                true);
#if PRINT_ENABLED
        fprintf(stderr, "WP: Segmented message.  Sending Abort!\n");
#endif
        goto WP_ABORT;
    }
    //check if previous wp is finished
    assert(!global_wp_data.inuse);
    /* decode the service request only */
    len = wp_decode_service_request(service_request, service_len, &wp_data);
#if PRINT_ENABLED
    if (len > 0)
        fprintf(stderr,
            "WP: type=%lu instance=%lu property=%lu priority=%lu index=%ld\n",
            (unsigned long) wp_data.object_type,
            (unsigned long) wp_data.object_instance,
            (unsigned long) wp_data.object_property,
            (unsigned long) wp_data.priority, (long) wp_data.array_index);
    else
        fprintf(stderr, "WP: Unable to decode Request!\n");
#endif
    /* bad decoding or something we didn't understand - send an abort */
    if (len <= 0) {
        len =
                abort_encode_apdu(&Handler_Transmit_Buffer[pdu_len],
                service_data->invoke_id, ABORT_REASON_OTHER, true);
#if PRINT_ENABLED
        fprintf(stderr, "WP: Bad Encoding. Sending Abort!\n");
#endif
        goto WP_ABORT;
    }
    val = Device_Write_Property(&wp_data);
    if (val == 1) {
        fprintf(stderr, "Receiver: Only 1 WP challenge is needed!\n");
        srand((unsigned int) time(NULL));
        //        random = rand();
        gen_rdm_bytestream(8, random);
        for (int i = 0; i < 8; ++i) {// store global random number
            global_random[i] = random[i];
        }
        len =
                encode_simple_ack_challenge(&Handler_Transmit_Buffer[pdu_len],
                service_data->invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY, &random[0], true);
#if PRINT_ENABLED
        fprintf(stderr, "WP: Sending 1 Challenge Request!\n");
#endif
    } else if (val > 1) {
        // TODO: generate random, send multiple challenges
        fprintf(stderr, "Receiver: #challenge requests = %d!\n", val);
        remain = val;
        srand((unsigned int) time(NULL));
        while (remain > 1) {
            tmp_pdu_len = pdu_len;
            // send random without FIN bit
            gen_rdm_bytestream(8, random);
            len =
                    encode_simple_ack_challenge(&Handler_Transmit_Buffer[tmp_pdu_len],
                    service_data->invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY, &random[0], false);
            tmp_pdu_len += len;
            bytes_sent =
                    datalink_send_pdu(src, &npdu_data, &Handler_Transmit_Buffer[0],
                    tmp_pdu_len);
            remain--;
        }
        gen_rdm_bytestream(8, random);
        tmp_pdu_len = pdu_len;
        for (int i = 0; i < 8; ++i) {// store global random number
            global_random[i] = random[i];
        }
        len =
                encode_simple_ack_challenge(&Handler_Transmit_Buffer[tmp_pdu_len],
                service_data->invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY, &random[0], true);
        tmp_pdu_len += len;
        bytes_sent =
                datalink_send_pdu(src, &npdu_data, &Handler_Transmit_Buffer[0],
                tmp_pdu_len);
        fprintf(stderr, "tmp_pdu_len is %d, bytes_sent in challenge request is %d\n", tmp_pdu_len, bytes_sent);
        fprintf(stderr, "Receiver: all WP challenge requests have been sent!\n");
        return;
    } else if (val == 0) {
        len =
                bacerror_encode_apdu(&Handler_Transmit_Buffer[pdu_len],
                service_data->invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY,
                wp_data.error_class, wp_data.error_code);
#if PRINT_ENABLED
        fprintf(stderr, "WP: Sending Error!\n");
#endif
    } else if (val == -1){
            len =
                bacerror_encode_apdu(&Handler_Transmit_Buffer[pdu_len],
                service_data->invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY,
                wp_data.error_class, wp_data.error_code);
            #if PRINT_ENABLED
        fprintf(stderr, "WP: Invalid WP value!\n");
#endif
    }
WP_ABORT:
    pdu_len += len;
    bytes_sent =
            datalink_send_pdu(src, &npdu_data, &Handler_Transmit_Buffer[0],
            pdu_len);
    fprintf(stderr, "pdu_len is %d, bytes_sent in challenge request is %d\n", pdu_len, bytes_sent);
#if PRINT_ENABLED
    if (bytes_sent <= 0) {
        fprintf(stderr, "WP: Failed to send PDU (%s)!\n", strerror(errno));
    }
#else
    bytes_sent = bytes_sent;
#endif

    return;
}

/** Perform basic validation of Write Property argument based on
 * the assumption that it is a string. Check for correct data type,
 * correct encoding (fixed here as ANSI X34),correct length, and
 * finally if it is allowed to be empty.
 */

bool WPValidateString(
        BACNET_APPLICATION_DATA_VALUE * pValue,
        int iMaxLen,
        bool bEmptyAllowed,
        BACNET_ERROR_CLASS * pErrorClass,
        BACNET_ERROR_CODE * pErrorCode) {
    bool bResult;

    /* Save on a bit of code duplication by pre selecting the most
     * common outcomes from the tests (not necessarily the most likely
     * outcome of the tests).
     */
    bResult = false;
    *pErrorClass = ERROR_CLASS_PROPERTY;

    if (pValue->tag == BACNET_APPLICATION_TAG_CHARACTER_STRING) {
        if (characterstring_encoding(&pValue->type.Character_String) ==
                CHARACTER_ANSI_X34) {
            if ((bEmptyAllowed == false) &&
                    (characterstring_length(&pValue->type.Character_String) ==
                    0)) {
                *pErrorCode = ERROR_CODE_VALUE_OUT_OF_RANGE;
            } else if ((bEmptyAllowed == false) &&
                    (!characterstring_printable(&pValue->type.Character_String))) {
                /* assumption: non-empty also means must be "printable" */
                *pErrorCode = ERROR_CODE_VALUE_OUT_OF_RANGE;
            } else if (characterstring_length(&pValue->type.Character_String) >
                    (uint16_t) iMaxLen) {
                *pErrorClass = ERROR_CLASS_RESOURCES;
                *pErrorCode = ERROR_CODE_NO_SPACE_TO_WRITE_PROPERTY;
            } else
                bResult = true; /* It's all good! */
        } else
            *pErrorCode = ERROR_CODE_CHARACTER_SET_NOT_SUPPORTED;
    } else
        *pErrorCode = ERROR_CODE_INVALID_DATA_TYPE;

    return (bResult);
}

/** Perform simple validation of type of Write Property argument based
 * the expected type vs the actual. Set up error response if the
 * validation fails. Cuts out reams of repeated code in the object code.
 */

bool WPValidateArgType(
        BACNET_APPLICATION_DATA_VALUE * pValue,
        uint8_t ucExpectedTag,
        BACNET_ERROR_CLASS * pErrorClass,
        BACNET_ERROR_CODE * pErrorCode) {
    bool bResult;

    /*
     * start out assuming success and only set up error
     * response if validation fails.
     */
    bResult = true;
    if (pValue->tag != ucExpectedTag) {
        bResult = false;
        *pErrorClass = ERROR_CLASS_PROPERTY;
        *pErrorCode = ERROR_CODE_INVALID_DATA_TYPE;
    }

    return (bResult);
}

///*compare if two random matches*/
//bool WPChallengeCompareHandler(
//        uint8_t * random) {
//    for (int i = 0; i < 8; ++i) {
//        if (random[i] != global_random[i]) {
//            return false;
//        }
//    }
//    return true;
//}
//
//bool WPExecuteCommand(uint8_t * random) {
//    bool match = false;
//    match = WPChallengeCompareHandler(random);
//    if (match) {
//        // execute av write prop command...
//        if (ExecuteWP(global_wp_data)) {
//            fprintf(stderr, "Succeed! WP is executed!\n");
//        } else {
//            fprintf(stderr, "Failed in WP Execution!\n");
//            return false;
//        }
//    } else {
//        fprintf(stderr, "WP: Random Number does not match!\n");
//        return false;
//    }
//
//    return true;
//}

void WP_Ack(BACNET_ADDRESS * src, uint8_t invoke_id) {
    int len = 0;
    int pdu_len = 0;
    BACNET_NPDU_DATA npdu_data;
    int bytes_sent = 0;
    BACNET_ADDRESS my_address;
    float val = (float) 0;

    /* encode the NPDU portion of the packet */
    datalink_get_my_address(&my_address);
    npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);
    memset(&Handler_Transmit_Buffer[0], 0, sizeof(Handler_Transmit_Buffer));
    pdu_len =
            npdu_encode_pdu(&Handler_Transmit_Buffer[0], src, &my_address,
            &npdu_data);

    len =
            encode_simple_ack(&Handler_Transmit_Buffer[pdu_len],
            invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY);

    pdu_len += len;
    fprintf(stderr, "lenth is %d!\n", pdu_len);
    bytes_sent =
            datalink_send_pdu(src, &npdu_data, &Handler_Transmit_Buffer[0],
            pdu_len);
    fprintf(stderr, "Sender: WP ACK has sent out! %d bytes\n", bytes_sent);
#if PRINT_ENABLED
    if (bytes_sent <= 0) {
        fprintf(stderr, "WP: Failed to send PDU (%s)!\n", strerror(errno));
    }
#else
    bytes_sent = bytes_sent;
#endif

    return;
}
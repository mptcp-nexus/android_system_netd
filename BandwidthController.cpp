/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0

/*
 * The CommandListener, FrameworkListener don't allow for
 * multiple calls in parallel to reach the BandwidthController.
 * If they ever were to allow it, then netd/ would need some tweaking.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>

#define LOG_TAG "BandwidthController"
#include <cutils/log.h>
#include <cutils/properties.h>

extern "C" int logwrap(int argc, const char **argv);
extern "C" int system_nosh(const char *command);

#include "NetdConstants.h"
#include "BandwidthController.h"

/* Alphabetical */
#define ALERT_IPT_TEMPLATE "%s %s %s -m quota2 ! --quota %lld --name %s"
const int  BandwidthController::ALERT_RULE_POS_IN_COSTLY_CHAIN = 4;
const char BandwidthController::ALERT_GLOBAL_NAME[] = "globalAlert";
const int  BandwidthController::MAX_CMD_ARGS = 32;
const int  BandwidthController::MAX_CMD_LEN = 1024;
const int  BandwidthController::MAX_IFACENAME_LEN = 64;
const int  BandwidthController::MAX_IPT_OUTPUT_LINE_LEN = 256;

bool BandwidthController::useLogwrapCall = false;

/**
 * Some comments about the rules:
 *  * Ordering
 *    - when an interface is marked as costly it should be INSERTED into the INPUT/OUTPUT chains.
 *      E.g. "-I bw_INPUT -i rmnet0 --jump costly"
 *    - quota'd rules in the costly chain should be before penalty_box lookups.
 *    - the qtaguid counting is done at the end of the bw_INPUT/bw_OUTPUT user chains.
 *
 * * global quota vs per interface quota
 *   - global quota for all costly interfaces uses a single costly chain:
 *    . initial rules
 *      iptables -N costly_shared
 *      iptables -I bw_INPUT -i iface0 --jump costly_shared
 *      iptables -I bw_OUTPUT -o iface0 --jump costly_shared
 *      iptables -I costly_shared -m quota \! --quota 500000 \
 *          --jump REJECT --reject-with icmp-net-prohibited
 *      iptables -A costly_shared --jump penalty_box
 *
 *    . adding a new iface to this, E.g.:
 *      iptables -I bw_INPUT -i iface1 --jump costly_shared
 *      iptables -I bw_OUTPUT -o iface1 --jump costly_shared
 *
 *   - quota per interface. This is achieve by having "costly" chains per quota.
 *     E.g. adding a new costly interface iface0 with its own quota:
 *      iptables -N costly_iface0
 *      iptables -I bw_INPUT -i iface0 --jump costly_iface0
 *      iptables -I bw_OUTPUT -o iface0 --jump costly_iface0
 *      iptables -A costly_iface0 -m quota \! --quota 500000 \
 *          --jump REJECT --reject-with icmp-net-prohibited
 *      iptables -A costly_iface0 --jump penalty_box
 *
 * * penalty_box handling:
 *  - only one penalty_box for all interfaces
 *   E.g  Adding an app:
 *    iptables -A penalty_box -m owner --uid-owner app_3 \
 *        --jump REJECT --reject-with icmp-net-prohibited
 */
const char *BandwidthController::IPT_FLUSH_COMMANDS[] = {
    /*
     * Cleanup rules.
     * Should normally include costly_<iface>, but we rely on the way they are setup
     * to allow coexistance.
     */
    "-F bw_INPUT",
    "-F bw_OUTPUT",
    "-F bw_FORWARD",
    "-F penalty_box",
    "-F costly_shared",

    "-t raw -F bw_raw_PREROUTING",
    "-t mangle -F bw_mangle_POSTROUTING",
};

/* The cleanup commands assume flushing has been done. */
const char *BandwidthController::IPT_CLEANUP_COMMANDS[] = {
    /* Delete hooks to custom chains. */
    "-D INPUT -j bw_INPUT",
    "-D OUTPUT -j bw_OUTPUT",
    "-D FORWARD -j bw_FORWARD",

    "-t raw -D bw_raw_PREROUTING",
    "-t mangle -D bw_mangle_POSTROUTING",

    "-X bw_INPUT",
    "-X bw_OUTPUT",
    "-X bw_FORWARD",
    "-X penalty_box",
    "-X costly_shared",

    "-t raw -X bw_raw_PREROUTING",
    "-t mangle -X bw_mangle_POSTROUTING",
};

const char *BandwidthController::IPT_SETUP_COMMANDS[] = {
    /* Created needed chains. */
    "-N bw_INPUT",
    "-A INPUT -j bw_INPUT",

    "-N bw_OUTPUT",
    "-A OUTPUT -j bw_OUTPUT",

    "-N bw_FORWARD",
    "-I FORWARD -j bw_FORWARD",

    "-N costly_shared",
    "-N penalty_box",

    "-t raw -N bw_raw_PREROUTING",
    "-t raw -A PREROUTING -j bw_raw_PREROUTING",
    "-t mangle -N bw_mangle_POSTROUTING",
    "-t mangle -A POSTROUTING -j bw_mangle_POSTROUTING",
};

const char *BandwidthController::IPT_BASIC_ACCOUNTING_COMMANDS[] = {
    "-A bw_INPUT -i lo --jump RETURN",
    "-A bw_INPUT -m owner --socket-exists", /* This is a tracking rule. */

    "-A bw_OUTPUT -o lo --jump RETURN",
    "-A bw_OUTPUT -m owner --socket-exists", /* This is a tracking rule. */

    "-A costly_shared --jump penalty_box",

    "-t raw -A bw_raw_PREROUTING ! -i lo+ -m owner --socket-exists", /* This is a tracking rule. */
    "-t mangle -A bw_mangle_POSTROUTING ! -o lo+ -m owner --socket-exists", /* This is a tracking rule. */
};

BandwidthController::BandwidthController(void) {
    char value[PROPERTY_VALUE_MAX];

    property_get("persist.bandwidth.uselogwrap", value, "0");
    useLogwrapCall = !strcmp(value, "1");
}

int BandwidthController::runIpxtablesCmd(const char *cmd, IptRejectOp rejectHandling,
                                         IptFailureLog failureHandling) {
    int res = 0;

    ALOGV("runIpxtablesCmd(cmd=%s)", cmd);
    res |= runIptablesCmd(cmd, rejectHandling, IptIpV4, failureHandling);
    res |= runIptablesCmd(cmd, rejectHandling, IptIpV6, failureHandling);
    return res;
}

int BandwidthController::StrncpyAndCheck(char *buffer, const char *src, size_t buffSize) {

    memset(buffer, '\0', buffSize);  // strncpy() is not filling leftover with '\0'
    strncpy(buffer, src, buffSize);
    return buffer[buffSize - 1];
}

int BandwidthController::runIptablesCmd(const char *cmd, IptRejectOp rejectHandling,
                                        IptIpVer iptVer, IptFailureLog failureHandling) {
    char buffer[MAX_CMD_LEN];
    const char *argv[MAX_CMD_ARGS];
    int argc = 0;
    char *next = buffer;
    char *tmp;
    int res;

    std::string fullCmd = cmd;

    if (rejectHandling == IptRejectAdd) {
        fullCmd += " --jump REJECT --reject-with";
        switch (iptVer) {
        case IptIpV4:
            fullCmd += " icmp-net-prohibited";
            break;
        case IptIpV6:
            fullCmd += " icmp6-adm-prohibited";
            break;
        }
    }

    fullCmd.insert(0, " ");
    fullCmd.insert(0, iptVer == IptIpV4 ? IPTABLES_PATH : IP6TABLES_PATH);

    if (!useLogwrapCall) {
        res = system_nosh(fullCmd.c_str());
    } else {
        if (StrncpyAndCheck(buffer, fullCmd.c_str(), sizeof(buffer))) {
            ALOGE("iptables command too long");
            return -1;
        }

        argc = 0;
        while ((tmp = strsep(&next, " "))) {
            argv[argc++] = tmp;
            if (argc >= MAX_CMD_ARGS) {
                ALOGE("iptables argument overflow");
                return -1;
            }
        }

        argv[argc] = NULL;
        res = logwrap(argc, argv);
    }
    if (res && failureHandling == IptFailShow) {
        ALOGE("runIptablesCmd(): failed %s res=%d", fullCmd.c_str(), res);
    }
    return res;
}

int BandwidthController::setupIptablesHooks(void) {

    /* Some of the initialCommands are allowed to fail */
    runCommands(sizeof(IPT_FLUSH_COMMANDS) / sizeof(char*),
            IPT_FLUSH_COMMANDS, RunCmdFailureOk);

    runCommands(sizeof(IPT_CLEANUP_COMMANDS) / sizeof(char*),
            IPT_CLEANUP_COMMANDS, RunCmdFailureOk);

    runCommands(sizeof(IPT_SETUP_COMMANDS) / sizeof(char*),
            IPT_SETUP_COMMANDS, RunCmdFailureBad);

    return 0;

}

int BandwidthController::enableBandwidthControl(bool force) {
    int res;
    char value[PROPERTY_VALUE_MAX];

    if (!force) {
            property_get("persist.bandwidth.enable", value, "1");
            if (!strcmp(value, "0"))
                    return 0;
    }

    /* Let's pretend we started from scratch ... */
    sharedQuotaIfaces.clear();
    quotaIfaces.clear();
    naughtyAppUids.clear();
    globalAlertBytes = 0;
    globalAlertTetherCount = 0;
    sharedQuotaBytes = sharedAlertBytes = 0;

    res = runCommands(sizeof(IPT_FLUSH_COMMANDS) / sizeof(char*),
            IPT_FLUSH_COMMANDS, RunCmdFailureOk);

    res |= runCommands(sizeof(IPT_BASIC_ACCOUNTING_COMMANDS) / sizeof(char*),
            IPT_BASIC_ACCOUNTING_COMMANDS, RunCmdFailureBad);

    return res;

}

int BandwidthController::disableBandwidthControl(void) {
    runCommands(sizeof(IPT_FLUSH_COMMANDS) / sizeof(char*),
            IPT_FLUSH_COMMANDS, RunCmdFailureOk);
    return 0;
}

int BandwidthController::runCommands(int numCommands, const char *commands[],
                                     RunCmdErrHandling cmdErrHandling) {
    int res = 0;
    IptFailureLog failureLogging = IptFailShow;
    if (cmdErrHandling == RunCmdFailureOk) {
        failureLogging = IptFailHide;
    }
    ALOGV("runCommands(): %d commands", numCommands);
    for (int cmdNum = 0; cmdNum < numCommands; cmdNum++) {
        res = runIpxtablesCmd(commands[cmdNum], IptRejectNoAdd, failureLogging);
        if (res && cmdErrHandling != RunCmdFailureOk)
            return res;
    }
    return 0;
}

std::string BandwidthController::makeIptablesNaughtyCmd(IptOp op, int uid) {
    std::string res;
    char *buff;
    const char *opFlag;

    switch (op) {
    case IptOpInsert:
        opFlag = "-I";
        break;
    case IptOpReplace:
        opFlag = "-R";
        break;
    default:
    case IptOpDelete:
        opFlag = "-D";
        break;
    }
    asprintf(&buff, "%s penalty_box -m owner --uid-owner %d", opFlag, uid);
    res = buff;
    free(buff);
    return res;
}

int BandwidthController::addNaughtyApps(int numUids, char *appUids[]) {
    return maninpulateNaughtyApps(numUids, appUids, NaughtyAppOpAdd);
}

int BandwidthController::removeNaughtyApps(int numUids, char *appUids[]) {
    return maninpulateNaughtyApps(numUids, appUids, NaughtyAppOpRemove);
}

int BandwidthController::maninpulateNaughtyApps(int numUids, char *appStrUids[], NaughtyAppOp appOp) {
    char cmd[MAX_CMD_LEN];
    int uidNum;
    const char *failLogTemplate;
    IptOp op;
    int appUids[numUids];
    std::string naughtyCmd;
    std::list<int /*uid*/>::iterator it;

    switch (appOp) {
    case NaughtyAppOpAdd:
        op = IptOpInsert;
        failLogTemplate = "Failed to add app uid %d to penalty box.";
        break;
    case NaughtyAppOpRemove:
        op = IptOpDelete;
        failLogTemplate = "Failed to delete app uid %d from penalty box.";
        break;
    default:
        ALOGE("Unexpected app Op %d", appOp);
        return -1;
    }

    for (uidNum = 0; uidNum < numUids; uidNum++) {
        appUids[uidNum] = atol(appStrUids[uidNum]);
        if (appUids[uidNum] == 0) {
            ALOGE(failLogTemplate, appUids[uidNum]);
            goto fail_parse;
        }
    }

    for (uidNum = 0; uidNum < numUids; uidNum++) {
        int uid = appUids[uidNum];
        for (it = naughtyAppUids.begin(); it != naughtyAppUids.end(); it++) {
            if (*it == uid)
                break;
        }
        bool found = (it != naughtyAppUids.end());

        if (appOp == NaughtyAppOpRemove) {
            if (!found) {
                ALOGE("No such appUid %d to remove", uid);
                return -1;
            }
            naughtyAppUids.erase(it);
        } else {
            if (found) {
                ALOGE("appUid %d exists already", uid);
                return -1;
            }
            naughtyAppUids.push_front(uid);
        }

        naughtyCmd = makeIptablesNaughtyCmd(op, uid);
        if (runIpxtablesCmd(naughtyCmd.c_str(), IptRejectAdd)) {
            ALOGE(failLogTemplate, uid);
            goto fail_with_uidNum;
        }
    }
    return 0;

fail_with_uidNum:
    /* Try to remove the uid that failed in any case*/
    naughtyCmd = makeIptablesNaughtyCmd(IptOpDelete, appUids[uidNum]);
    runIpxtablesCmd(naughtyCmd.c_str(), IptRejectAdd);
fail_parse:
    return -1;
}

std::string BandwidthController::makeIptablesQuotaCmd(IptOp op, const char *costName, int64_t quota) {
    std::string res;
    char *buff;
    const char *opFlag;

    ALOGV("makeIptablesQuotaCmd(%d, %lld)", op, quota);

    switch (op) {
    case IptOpInsert:
        opFlag = "-I";
        break;
    case IptOpReplace:
        opFlag = "-R";
        break;
    default:
    case IptOpDelete:
        opFlag = "-D";
        break;
    }

    // The requried IP version specific --jump REJECT ... will be added later.
    asprintf(&buff, "%s costly_%s -m quota2 ! --quota %lld --name %s", opFlag, costName, quota,
             costName);
    res = buff;
    free(buff);
    return res;
}

int BandwidthController::prepCostlyIface(const char *ifn, QuotaType quotaType) {
    char cmd[MAX_CMD_LEN];
    int res = 0, res1, res2;
    int ruleInsertPos = 1;
    std::string costString;
    const char *costCString;

    /* The "-N costly" is created upfront, no need to handle it here. */
    switch (quotaType) {
    case QuotaUnique:
        costString = "costly_";
        costString += ifn;
        costCString = costString.c_str();
        /*
         * Flush the costly_<iface> is allowed to fail in case it didn't exist.
         * Creating a new one is allowed to fail in case it existed.
         * This helps with netd restarts.
         */
        snprintf(cmd, sizeof(cmd), "-F %s", costCString);
        res1 = runIpxtablesCmd(cmd, IptRejectNoAdd, IptFailHide);
        snprintf(cmd, sizeof(cmd), "-N %s", costCString);
        res2 = runIpxtablesCmd(cmd, IptRejectNoAdd, IptFailHide);
        res = (res1 && res2) || (!res1 && !res2);

        snprintf(cmd, sizeof(cmd), "-A %s -j penalty_box", costCString);
        res |= runIpxtablesCmd(cmd, IptRejectNoAdd);
        break;
    case QuotaShared:
        costCString = "costly_shared";
        break;
    default:
        ALOGE("Unexpected quotatype %d", quotaType);
        return -1;
    }

    if (globalAlertBytes) {
        /* The alert rule comes 1st */
        ruleInsertPos = 2;
    }

    snprintf(cmd, sizeof(cmd), "-D bw_INPUT -i %s --jump %s", ifn, costCString);
    runIpxtablesCmd(cmd, IptRejectNoAdd, IptFailHide);

    snprintf(cmd, sizeof(cmd), "-I bw_INPUT %d -i %s --jump %s", ruleInsertPos, ifn, costCString);
    res |= runIpxtablesCmd(cmd, IptRejectNoAdd);

    snprintf(cmd, sizeof(cmd), "-D bw_OUTPUT -o %s --jump %s", ifn, costCString);
    runIpxtablesCmd(cmd, IptRejectNoAdd, IptFailHide);

    snprintf(cmd, sizeof(cmd), "-I bw_OUTPUT %d -o %s --jump %s", ruleInsertPos, ifn, costCString);
    res |= runIpxtablesCmd(cmd, IptRejectNoAdd);
    return res;
}

int BandwidthController::cleanupCostlyIface(const char *ifn, QuotaType quotaType) {
    char cmd[MAX_CMD_LEN];
    int res = 0;
    std::string costString;
    const char *costCString;

    switch (quotaType) {
    case QuotaUnique:
        costString = "costly_";
        costString += ifn;
        costCString = costString.c_str();
        break;
    case QuotaShared:
        costCString = "costly_shared";
        break;
    default:
        ALOGE("Unexpected quotatype %d", quotaType);
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "-D bw_INPUT -i %s --jump %s", ifn, costCString);
    res |= runIpxtablesCmd(cmd, IptRejectNoAdd);
    snprintf(cmd, sizeof(cmd), "-D bw_OUTPUT -o %s --jump %s", ifn, costCString);
    res |= runIpxtablesCmd(cmd, IptRejectNoAdd);

    /* The "-N costly_shared" is created upfront, no need to handle it here. */
    if (quotaType == QuotaUnique) {
        snprintf(cmd, sizeof(cmd), "-F %s", costCString);
        res |= runIpxtablesCmd(cmd, IptRejectNoAdd);
        snprintf(cmd, sizeof(cmd), "-X %s", costCString);
        res |= runIpxtablesCmd(cmd, IptRejectNoAdd);
    }
    return res;
}

int BandwidthController::setInterfaceSharedQuota(const char *iface, int64_t maxBytes) {
    char cmd[MAX_CMD_LEN];
    char ifn[MAX_IFACENAME_LEN];
    int res = 0;
    std::string quotaCmd;
    std::string ifaceName;
    ;
    const char *costName = "shared";
    std::list<std::string>::iterator it;

    if (!maxBytes) {
        /* Don't talk about -1, deprecate it. */
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    if (StrncpyAndCheck(ifn, iface, sizeof(ifn))) {
        ALOGE("Interface name longer than %d", MAX_IFACENAME_LEN);
        return -1;
    }
    ifaceName = ifn;

    if (maxBytes == -1) {
        return removeInterfaceSharedQuota(ifn);
    }

    /* Insert ingress quota. */
    for (it = sharedQuotaIfaces.begin(); it != sharedQuotaIfaces.end(); it++) {
        if (*it == ifaceName)
            break;
    }

    if (it == sharedQuotaIfaces.end()) {
        res |= prepCostlyIface(ifn, QuotaShared);
        if (sharedQuotaIfaces.empty()) {
            quotaCmd = makeIptablesQuotaCmd(IptOpInsert, costName, maxBytes);
            res |= runIpxtablesCmd(quotaCmd.c_str(), IptRejectAdd);
            if (res) {
                ALOGE("Failed set quota rule");
                goto fail;
            }
            sharedQuotaBytes = maxBytes;
        }
        sharedQuotaIfaces.push_front(ifaceName);

    }

    if (maxBytes != sharedQuotaBytes) {
        res |= updateQuota(costName, maxBytes);
        if (res) {
            ALOGE("Failed update quota for %s", costName);
            goto fail;
        }
        sharedQuotaBytes = maxBytes;
    }
    return 0;

    fail:
    /*
     * TODO(jpa): once we get rid of iptables in favor of rtnetlink, reparse
     * rules in the kernel to see which ones need cleaning up.
     * For now callers needs to choose if they want to "ndc bandwidth enable"
     * which resets everything.
     */
    removeInterfaceSharedQuota(ifn);
    return -1;
}

/* It will also cleanup any shared alerts */
int BandwidthController::removeInterfaceSharedQuota(const char *iface) {
    char ifn[MAX_IFACENAME_LEN];
    int res = 0;
    std::string ifaceName;
    std::list<std::string>::iterator it;
    const char *costName = "shared";

    if (StrncpyAndCheck(ifn, iface, sizeof(ifn))) {
        ALOGE("Interface name longer than %d", MAX_IFACENAME_LEN);
        return -1;
    }
    ifaceName = ifn;

    for (it = sharedQuotaIfaces.begin(); it != sharedQuotaIfaces.end(); it++) {
        if (*it == ifaceName)
            break;
    }
    if (it == sharedQuotaIfaces.end()) {
        ALOGE("No such iface %s to delete", ifn);
        return -1;
    }

    res |= cleanupCostlyIface(ifn, QuotaShared);
    sharedQuotaIfaces.erase(it);

    if (sharedQuotaIfaces.empty()) {
        std::string quotaCmd;
        quotaCmd = makeIptablesQuotaCmd(IptOpDelete, costName, sharedQuotaBytes);
        res |= runIpxtablesCmd(quotaCmd.c_str(), IptRejectAdd);
        sharedQuotaBytes = 0;
        if (sharedAlertBytes) {
            removeSharedAlert();
            sharedAlertBytes = 0;
        }
    }
    return res;
}

int BandwidthController::setInterfaceQuota(const char *iface, int64_t maxBytes) {
    char ifn[MAX_IFACENAME_LEN];
    int res = 0;
    std::string ifaceName;
    const char *costName;
    std::list<QuotaInfo>::iterator it;
    std::string quotaCmd;

    if (!maxBytes) {
        /* Don't talk about -1, deprecate it. */
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    if (maxBytes == -1) {
        return removeInterfaceQuota(iface);
    }

    if (StrncpyAndCheck(ifn, iface, sizeof(ifn))) {
        ALOGE("Interface name longer than %d", MAX_IFACENAME_LEN);
        return -1;
    }
    ifaceName = ifn;
    costName = iface;

    /* Insert ingress quota. */
    for (it = quotaIfaces.begin(); it != quotaIfaces.end(); it++) {
        if (it->ifaceName == ifaceName)
            break;
    }

    if (it == quotaIfaces.end()) {
        res |= prepCostlyIface(ifn, QuotaUnique);
        quotaCmd = makeIptablesQuotaCmd(IptOpInsert, costName, maxBytes);
        res |= runIpxtablesCmd(quotaCmd.c_str(), IptRejectAdd);
        if (res) {
            ALOGE("Failed set quota rule");
            goto fail;
        }

        quotaIfaces.push_front(QuotaInfo(ifaceName, maxBytes, 0));

    } else {
        res |= updateQuota(costName, maxBytes);
        if (res) {
            ALOGE("Failed update quota for %s", iface);
            goto fail;
        }
        it->quota = maxBytes;
    }
    return 0;

    fail:
    /*
     * TODO(jpa): once we get rid of iptables in favor of rtnetlink, reparse
     * rules in the kernel to see which ones need cleaning up.
     * For now callers needs to choose if they want to "ndc bandwidth enable"
     * which resets everything.
     */
    removeInterfaceSharedQuota(ifn);
    return -1;
}

int BandwidthController::getInterfaceSharedQuota(int64_t *bytes) {
    return getInterfaceQuota("shared", bytes);
}

int BandwidthController::getInterfaceQuota(const char *costName, int64_t *bytes) {
    FILE *fp;
    char *fname;
    int scanRes;

    asprintf(&fname, "/proc/net/xt_quota/%s", costName);
    fp = fopen(fname, "r");
    free(fname);
    if (!fp) {
        ALOGE("Reading quota %s failed (%s)", costName, strerror(errno));
        return -1;
    }
    scanRes = fscanf(fp, "%lld", bytes);
    ALOGV("Read quota res=%d bytes=%lld", scanRes, *bytes);
    fclose(fp);
    return scanRes == 1 ? 0 : -1;
}

int BandwidthController::removeInterfaceQuota(const char *iface) {

    char ifn[MAX_IFACENAME_LEN];
    int res = 0;
    std::string ifaceName;
    const char *costName;
    std::list<QuotaInfo>::iterator it;

    if (StrncpyAndCheck(ifn, iface, sizeof(ifn))) {
        ALOGE("Interface name longer than %d", MAX_IFACENAME_LEN);
        return -1;
    }
    ifaceName = ifn;
    costName = iface;

    for (it = quotaIfaces.begin(); it != quotaIfaces.end(); it++) {
        if (it->ifaceName == ifaceName)
            break;
    }

    if (it == quotaIfaces.end()) {
        ALOGE("No such iface %s to delete", ifn);
        return -1;
    }

    /* This also removes the quota command of CostlyIface chain. */
    res |= cleanupCostlyIface(ifn, QuotaUnique);

    quotaIfaces.erase(it);

    return res;
}

int BandwidthController::updateQuota(const char *quotaName, int64_t bytes) {
    FILE *fp;
    char *fname;

    asprintf(&fname, "/proc/net/xt_quota/%s", quotaName);
    fp = fopen(fname, "w");
    free(fname);
    if (!fp) {
        ALOGE("Updating quota %s failed (%s)", quotaName, strerror(errno));
        return -1;
    }
    fprintf(fp, "%lld\n", bytes);
    fclose(fp);
    return 0;
}

int BandwidthController::runIptablesAlertCmd(IptOp op, const char *alertName, int64_t bytes) {
    int res = 0;
    const char *opFlag;
    const char *ifaceLimiting;
    char *alertQuotaCmd;

    switch (op) {
    case IptOpInsert:
        opFlag = "-I";
        break;
    case IptOpReplace:
        opFlag = "-R";
        break;
    default:
    case IptOpDelete:
        opFlag = "-D";
        break;
    }

    ifaceLimiting = "! -i lo+";
    asprintf(&alertQuotaCmd, ALERT_IPT_TEMPLATE, ifaceLimiting, opFlag, "bw_INPUT",
        bytes, alertName);
    res |= runIpxtablesCmd(alertQuotaCmd, IptRejectNoAdd);
    free(alertQuotaCmd);
    ifaceLimiting = "! -o lo+";
    asprintf(&alertQuotaCmd, ALERT_IPT_TEMPLATE, ifaceLimiting, opFlag, "bw_OUTPUT",
        bytes, alertName);
    res |= runIpxtablesCmd(alertQuotaCmd, IptRejectNoAdd);
    free(alertQuotaCmd);
    return res;
}

int BandwidthController::runIptablesAlertFwdCmd(IptOp op, const char *alertName, int64_t bytes) {
    int res = 0;
    const char *opFlag;
    const char *ifaceLimiting;
    char *alertQuotaCmd;

    switch (op) {
    case IptOpInsert:
        opFlag = "-I";
        break;
    case IptOpReplace:
        opFlag = "-R";
        break;
    default:
    case IptOpDelete:
        opFlag = "-D";
        break;
    }

    ifaceLimiting = "! -i lo+";
    asprintf(&alertQuotaCmd, ALERT_IPT_TEMPLATE, ifaceLimiting, opFlag, "bw_FORWARD",
        bytes, alertName);
    res = runIpxtablesCmd(alertQuotaCmd, IptRejectNoAdd);
    free(alertQuotaCmd);
    return res;
}

int BandwidthController::setGlobalAlert(int64_t bytes) {
    const char *alertName = ALERT_GLOBAL_NAME;
    int res = 0;

    if (!bytes) {
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    if (globalAlertBytes) {
        res = updateQuota(alertName, bytes);
    } else {
        res = runIptablesAlertCmd(IptOpInsert, alertName, bytes);
        if (globalAlertTetherCount) {
            ALOGV("setGlobalAlert for %d tether", globalAlertTetherCount);
            res |= runIptablesAlertFwdCmd(IptOpInsert, alertName, bytes);
        }
    }
    globalAlertBytes = bytes;
    return res;
}

int BandwidthController::setGlobalAlertInForwardChain(void) {
    const char *alertName = ALERT_GLOBAL_NAME;
    int res = 0;

    globalAlertTetherCount++;
    ALOGV("setGlobalAlertInForwardChain(): %d tether", globalAlertTetherCount);

    /*
     * If there is no globalAlert active we are done.
     * If there is an active globalAlert but this is not the 1st
     * tether, we are also done.
     */
    if (!globalAlertBytes || globalAlertTetherCount != 1) {
        return 0;
    }

    /* We only add the rule if this was the 1st tether added. */
    res = runIptablesAlertFwdCmd(IptOpInsert, alertName, globalAlertBytes);
    return res;
}

int BandwidthController::removeGlobalAlert(void) {

    const char *alertName = ALERT_GLOBAL_NAME;
    int res = 0;

    if (!globalAlertBytes) {
        ALOGE("No prior alert set");
        return -1;
    }
    res = runIptablesAlertCmd(IptOpDelete, alertName, globalAlertBytes);
    if (globalAlertTetherCount) {
        res |= runIptablesAlertFwdCmd(IptOpDelete, alertName, globalAlertBytes);
    }
    globalAlertBytes = 0;
    return res;
}

int BandwidthController::removeGlobalAlertInForwardChain(void) {
    int res = 0;
    const char *alertName = ALERT_GLOBAL_NAME;

    if (!globalAlertTetherCount) {
        ALOGE("No prior alert set");
        return -1;
    }

    globalAlertTetherCount--;
    /*
     * If there is no globalAlert active we are done.
     * If there is an active globalAlert but there are more
     * tethers, we are also done.
     */
    if (!globalAlertBytes || globalAlertTetherCount >= 1) {
        return 0;
    }

    /* We only detete the rule if this was the last tether removed. */
    res = runIptablesAlertFwdCmd(IptOpDelete, alertName, globalAlertBytes);
    return res;
}

int BandwidthController::setSharedAlert(int64_t bytes) {
    if (!sharedQuotaBytes) {
        ALOGE("Need to have a prior shared quota set to set an alert");
        return -1;
    }
    if (!bytes) {
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    return setCostlyAlert("shared", bytes, &sharedAlertBytes);
}

int BandwidthController::removeSharedAlert(void) {
    return removeCostlyAlert("shared", &sharedAlertBytes);
}

int BandwidthController::setInterfaceAlert(const char *iface, int64_t bytes) {
    std::list<QuotaInfo>::iterator it;

    if (!bytes) {
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    for (it = quotaIfaces.begin(); it != quotaIfaces.end(); it++) {
        if (it->ifaceName == iface)
            break;
    }

    if (it == quotaIfaces.end()) {
        ALOGE("Need to have a prior interface quota set to set an alert");
        return -1;
    }

    return setCostlyAlert(iface, bytes, &it->alert);
}

int BandwidthController::removeInterfaceAlert(const char *iface) {
    std::list<QuotaInfo>::iterator it;

    for (it = quotaIfaces.begin(); it != quotaIfaces.end(); it++) {
        if (it->ifaceName == iface)
            break;
    }

    if (it == quotaIfaces.end()) {
        ALOGE("No prior alert set for interface %s", iface);
        return -1;
    }

    return removeCostlyAlert(iface, &it->alert);
}

int BandwidthController::setCostlyAlert(const char *costName, int64_t bytes, int64_t *alertBytes) {
    char *alertQuotaCmd;
    char *chainNameAndPos;
    int res = 0;
    char *alertName;

    if (!bytes) {
        ALOGE("Invalid bytes value. 1..max_int64.");
        return -1;
    }
    asprintf(&alertName, "%sAlert", costName);
    if (*alertBytes) {
        res = updateQuota(alertName, *alertBytes);
    } else {
        asprintf(&chainNameAndPos, "costly_%s %d", costName, ALERT_RULE_POS_IN_COSTLY_CHAIN);
        asprintf(&alertQuotaCmd, ALERT_IPT_TEMPLATE, "", "-I", chainNameAndPos, bytes, alertName);
        res |= runIpxtablesCmd(alertQuotaCmd, IptRejectNoAdd);
        free(alertQuotaCmd);
        free(chainNameAndPos);
    }
    *alertBytes = bytes;
    free(alertName);
    return res;
}

int BandwidthController::removeCostlyAlert(const char *costName, int64_t *alertBytes) {
    char *alertQuotaCmd;
    char *chainName;
    char *alertName;
    int res = 0;

    asprintf(&alertName, "%sAlert", costName);
    if (!*alertBytes) {
        ALOGE("No prior alert set for %s alert", costName);
        return -1;
    }

    asprintf(&chainName, "costly_%s", costName);
    asprintf(&alertQuotaCmd, ALERT_IPT_TEMPLATE, "", "-D", chainName, *alertBytes, alertName);
    res |= runIpxtablesCmd(alertQuotaCmd, IptRejectNoAdd);
    free(alertQuotaCmd);
    free(chainName);

    *alertBytes = 0;
    free(alertName);
    return res;
}

/*
 * Parse the ptks and bytes out of:
 * Chain FORWARD (policy RETURN 0 packets, 0 bytes)
 *     pkts      bytes target     prot opt in     out     source               destination
 *        0        0 RETURN     all  --  rmnet0 wlan0   0.0.0.0/0            0.0.0.0/0            state RELATED,ESTABLISHED
 *        0        0 DROP       all  --  wlan0  rmnet0  0.0.0.0/0            0.0.0.0/0            state INVALID
 *        0        0 RETURN     all  --  wlan0  rmnet0  0.0.0.0/0            0.0.0.0/0
 *
 */
int BandwidthController::parseForwardChainStats(TetherStats &stats, FILE *fp,
                                                std::string &extraProcessingInfo) {
    int res;
    char lineBuffer[MAX_IPT_OUTPUT_LINE_LEN];
    char iface0[MAX_IPT_OUTPUT_LINE_LEN];
    char iface1[MAX_IPT_OUTPUT_LINE_LEN];
    char rest[MAX_IPT_OUTPUT_LINE_LEN];

    char *buffPtr;
    int64_t packets, bytes;

    while (NULL != (buffPtr = fgets(lineBuffer, MAX_IPT_OUTPUT_LINE_LEN, fp))) {
        /* Clean up, so a failed parse can still print info */
        iface0[0] = iface1[0] = rest[0] = packets = bytes = 0;
        res = sscanf(buffPtr, "%lld %lld RETURN all -- %s %s 0.%s",
                &packets, &bytes, iface0, iface1, rest);
        ALOGV("parse res=%d iface0=<%s> iface1=<%s> pkts=%lld bytes=%lld rest=<%s> orig line=<%s>", res,
             iface0, iface1, packets, bytes, rest, buffPtr);
        extraProcessingInfo += buffPtr;

        if (res != 5) {
            continue;
        }
        if ((stats.ifaceIn == iface0) && (stats.ifaceOut == iface1)) {
            ALOGV("iface_in=%s iface_out=%s rx_bytes=%lld rx_packets=%lld ", iface0, iface1, bytes, packets);
            stats.rxPackets = packets;
            stats.rxBytes = bytes;
        } else if ((stats.ifaceOut == iface0) && (stats.ifaceIn == iface1)) {
            ALOGV("iface_in=%s iface_out=%s tx_bytes=%lld tx_packets=%lld ", iface1, iface0, bytes, packets);
            stats.txPackets = packets;
            stats.txBytes = bytes;
        }
    }
    /* Failure if rx or tx was not found */
    return (stats.rxBytes == -1 || stats.txBytes == -1) ? -1 : 0;
}


char *BandwidthController::TetherStats::getStatsLine(void) {
    char *msg;
    asprintf(&msg, "%s %s %lld %lld %lld %lld", ifaceIn.c_str(), ifaceOut.c_str(),
            rxBytes, rxPackets, txBytes, txPackets);
    return msg;
}

int BandwidthController::getTetherStats(TetherStats &stats, std::string &extraProcessingInfo) {
    int res;
    std::string fullCmd;
    FILE *iptOutput;
    const char *cmd;

    if (stats.rxBytes != -1 || stats.txBytes != -1) {
        ALOGE("Unexpected input stats. Byte counts should be -1.");
        return -1;
    }

    /*
     * Why not use some kind of lib to talk to iptables?
     * Because the only libs are libiptc and libip6tc in iptables, and they are
     * not easy to use. They require the known iptables match modules to be
     * preloaded/linked, and require apparently a lot of wrapper code to get
     * the wanted info.
     */
    fullCmd = IPTABLES_PATH;
    fullCmd += " -nvx -L natctrl_FORWARD";
    iptOutput = popen(fullCmd.c_str(), "r");
    if (!iptOutput) {
            ALOGE("Failed to run %s err=%s", fullCmd.c_str(), strerror(errno));
            extraProcessingInfo += "Failed to run iptables.";
        return -1;
    }
    res = parseForwardChainStats(stats, iptOutput, extraProcessingInfo);
    pclose(iptOutput);

    /* Currently NatController doesn't do ipv6 tethering, so we are done. */
    return res;
}

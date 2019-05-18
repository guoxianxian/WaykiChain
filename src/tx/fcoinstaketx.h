// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php

#ifndef FCOIN_STAKE_H
#define FCOIN_STAKE_H

#include "tx.h"

class CFcoinStakeTx: CBaseTx {
public:
    uint64_t stake_fcoins;

};
#endif
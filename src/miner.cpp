// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2018-2019 The Safe Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "config/safe-chain.h"
#include "miner.h"

#include "amount.h"
#include "chain.h"
#include "chainparams.h"
#include "coins.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "hash.h"
#include "validation.h"
#include "net.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "timedata.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "validationinterface.h"
#include "masternodeman.h"
#include "main.h"
#include "activemasternode.h"
#include "messagesigner.h"

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/lexical_cast.hpp>

#include <queue>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// SafeMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
extern int g_nStartSPOSHeight;
extern CMasternodeMan mnodeman;
extern CActiveMasternode activeMasternode;
extern unsigned int g_nMasternodeSPosCount;
extern unsigned int g_nMasternodeCanBeSelectedTime;
extern int64_t g_nStartNewLoopTimeMS;
extern std::vector<CMasternode> g_vecResultMasternodes;
extern std::map<CNetAddr, LocalServiceInfo> mapLocalHost;
extern CCriticalSection cs_spos;
extern unsigned int g_nAllowableErrorTime;
extern int g_nTimeoutPushForwardHeight;
extern int g_nMinerBlockTimeout;
extern int g_nSelectGlobalDefaultValue;
extern int g_nPushForwardHeight;
extern int g_nTimeoutCount;
extern int g_nMaxTimeoutCount;
extern unsigned int g_nMasternodeSPosCount;
extern unsigned int g_nMasternodeMinCount;
extern int g_nPushForwardTime;
extern bool g_fReceiveBlock;
uint32_t nSposSleeptime = 50;




class ScoreCompare
{
public:
    ScoreCompare() {}

    bool operator()(const CTxMemPool::txiter a, const CTxMemPool::txiter b)
    {
        return CompareTxMemPoolEntryByScore()(*b,*a); // Convert to less than
    }
};

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());
    if(!IsStartSPosHeight(pindexPrev->nHeight+1))
    {
        if (nOldTime < nNewTime)
            pblock->nTime = nNewTime;
    }

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks&&!IsStartSPosHeight(pindexPrev->nHeight+1))
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);

    return nNewTime - nOldTime;
}

CBlockTemplate* CreateNewBlock(const CChainParams& chainparams, const CScript& scriptPubKeyIn)
{
    // Create new block
    std::unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())
        return NULL;

    CBlock *pblock = &pblocktemplate->block; // pointer for convenience

    // Create coinbase tx
    CMutableTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);
    txNew.vout[0].scriptPubKey = scriptPubKeyIn;

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to between 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MaxBlockSize(fDIP0001ActiveAtTip)-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    CTxMemPool::setEntries inBlock;
    CTxMemPool::setEntries waitSet;

    // This vector will be sorted into a priority queue:
    vector<TxCoinAgePriority> vecPriority;
    TxCoinAgePriorityCompare pricomparer;
    std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash> waitPriMap;
    typedef std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash>::iterator waitPriIter;
    double actualPriority = -1;

    std::priority_queue<CTxMemPool::txiter, std::vector<CTxMemPool::txiter>, ScoreCompare> clearedTxs;
    bool fPrintPriority = GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    uint64_t nBlockSize = 1000;
    uint64_t nBlockTx = 0;
    unsigned int nBlockSigOps = 100;
    int lastFewTxs = 0;
    CAmount nFees = 0;

    {
        LOCK(cs_main);

        CBlockIndex* pindexPrev = chainActive.Tip();
        const int nHeight = pindexPrev->nHeight + 1;
        pblock->nTime = GetAdjustedTime();
        const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

        // Add our coinbase tx as first transaction
        pblock->vtx.push_back(txNew);
        pblocktemplate->vTxFees.push_back(-1); // updated at end
        pblocktemplate->vTxSigOps.push_back(-1); // updated at end
        pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
        // -regtest only: allow overriding block.nVersion with
        // -blockversion=N to test forking scenarios
        if (chainparams.MineBlocksOnDemand())
            pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

        int64_t nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                                ? nMedianTimePast
                                : pblock->GetBlockTime();

        {
            LOCK(mempool.cs);

            bool fPriorityBlock = nBlockPrioritySize > 0;
            if (fPriorityBlock) {
                vecPriority.reserve(mempool.mapTx.size());
                for (CTxMemPool::indexed_transaction_set::iterator mi = mempool.mapTx.begin();
                     mi != mempool.mapTx.end(); ++mi)
                {
                    double dPriority = mi->GetPriority(nHeight);
                    CAmount dummy;
                    mempool.ApplyDeltas(mi->GetTx().GetHash(), dPriority, dummy);
                    vecPriority.push_back(TxCoinAgePriority(dPriority, mi));
                }
                std::make_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
            }

            CTxMemPool::indexed_transaction_set::nth_index<3>::type::iterator mi = mempool.mapTx.get<3>().begin();
            CTxMemPool::txiter iter;

            while (mi != mempool.mapTx.get<3>().end() || !clearedTxs.empty())
            {
                bool priorityTx = false;
                if (fPriorityBlock && !vecPriority.empty()) { // add a tx from priority queue to fill the blockprioritysize
                    priorityTx = true;
                    iter = vecPriority.front().second;
                    actualPriority = vecPriority.front().first;
                    std::pop_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
                    vecPriority.pop_back();
                }
                else if (clearedTxs.empty()) { // add tx with next highest score
                    iter = mempool.mapTx.project<0>(mi);
                    mi++;
                }
                else {  // try to add a previously postponed child tx
                    iter = clearedTxs.top();
                    clearedTxs.pop();
                }

                if (inBlock.count(iter))
                    continue; // could have been added to the priorityBlock

                const CTransaction& tx = iter->GetTx();

                bool fOrphan = false;
                BOOST_FOREACH(CTxMemPool::txiter parent, mempool.GetMemPoolParents(iter))
                {
                    if (!inBlock.count(parent)) {
                        fOrphan = true;
                        break;
                    }
                }
                if (fOrphan) {
                    if (priorityTx)
                        waitPriMap.insert(std::make_pair(iter,actualPriority));
                    else
                        waitSet.insert(iter);
                    continue;
                }

                unsigned int nTxSize = iter->GetTxSize();
                if (fPriorityBlock &&
                    (nBlockSize + nTxSize >= nBlockPrioritySize || !AllowFree(actualPriority))) {
                    fPriorityBlock = false;
                    waitPriMap.clear();
                }
                if (!priorityTx &&
                    (iter->GetModifiedFee() < ::minRelayTxFee.GetFee(nTxSize) && nBlockSize >= nBlockMinSize)) {
                    break;
                }
                if (nBlockSize + nTxSize >= nBlockMaxSize) {
                    if (nBlockSize >  nBlockMaxSize - 100 || lastFewTxs > 50) {
                        break;
                    }
                    // Once we're within 1000 bytes of a full block, only look at 50 more txs
                    // to try to fill the remaining space.
                    if (nBlockSize > nBlockMaxSize - 1000) {
                        lastFewTxs++;
                    }
                    continue;
                }

                if (!IsFinalTx(tx, nHeight, nLockTimeCutoff))
                    continue;

                unsigned int nTxSigOps = iter->GetSigOpCount();
                unsigned int nMaxBlockSigOps = MaxBlockSigOps(fDIP0001ActiveAtTip);
                if (nBlockSigOps + nTxSigOps >= nMaxBlockSigOps) {
                    if (nBlockSigOps > nMaxBlockSigOps - 2) {
                        break;
                    }
                    continue;
                }

                CAmount nTxFees = iter->GetFee();
                // Added
                pblock->vtx.push_back(tx);
                pblocktemplate->vTxFees.push_back(nTxFees);
                pblocktemplate->vTxSigOps.push_back(nTxSigOps);
                nBlockSize += nTxSize;
                ++nBlockTx;
                nBlockSigOps += nTxSigOps;
                nFees += nTxFees;

                if (fPrintPriority)
                {
                    double dPriority = iter->GetPriority(nHeight);
                    CAmount dummy;
                    mempool.ApplyDeltas(tx.GetHash(), dPriority, dummy);
                    LogPrintf("priority %.1f fee %s txid %s\n",
                              dPriority , CFeeRate(iter->GetModifiedFee(), nTxSize).ToString(), tx.GetHash().ToString());
                }

                inBlock.insert(iter);

                // Add transactions that depend on this one to the priority queue
                BOOST_FOREACH(CTxMemPool::txiter child, mempool.GetMemPoolChildren(iter))
                {
                    if (fPriorityBlock) {
                        waitPriIter wpiter = waitPriMap.find(child);
                        if (wpiter != waitPriMap.end()) {
                            vecPriority.push_back(TxCoinAgePriority(wpiter->second,child));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
                            waitPriMap.erase(wpiter);
                        }
                    }
                    else {
                        if (waitSet.count(child)) {
                            clearedTxs.push(child);
                            waitSet.erase(child);
                        }
                    }
                }
            }
        }

        // NOTE: unlike in bitcoin, we need to pass PREVIOUS block height here
        CAmount blockReward = 0;
        if (nHeight >= g_nStartSPOSHeight)
        {
            masternode_info_t mnInfoRet;
            if(!mnodeman.GetMasternodeInfo(activeMasternode.outpoint,mnInfoRet))
            {
                LogPrintf("SPOS_Warning:create block not find the outpoint(%s),maybe need to start alias or check the masternode list\n",activeMasternode.outpoint.ToString());
                return NULL;
            }

            CScript sposMinerPayee = GetScriptForDestination(mnInfoRet.pubKeyCollateralAddress.GetID());
            txNew.vout[0].scriptPubKey = sposMinerPayee;
            blockReward = nFees + GetSPOSBlockSubsidy(pindexPrev->nHeight, Params().GetConsensus());
        }
        else
            blockReward = nFees + GetBlockSubsidy(pindexPrev->nBits, pindexPrev->nHeight, Params().GetConsensus());

        // Compute regular coinbase transaction.
        txNew.vout[0].nValue = blockReward;
        txNew.vin[0].scriptSig = CScript() << nHeight << OP_0;

        // Update coinbase transaction with additional info about masternode and governance payments,
        // get some info back to pass to getblocktemplate
        FillBlockPayments(txNew, nHeight, blockReward, pblock->txoutMasternode, pblock->voutSuperblock);
        // LogPrintf("CreateNewBlock -- nBlockHeight %d blockReward %lld txoutMasternode %s txNew %s",
        //             nHeight, blockReward, pblock->txoutMasternode.ToString(), txNew.ToString());

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        LogPrintf("CreateNewBlock(): total size %u txs: %u fees: %ld sigops %d\n", nBlockSize, nBlockTx, nFees, nBlockSigOps);

        // Update block coinbase
        pblock->vtx[0] = txNew;
        pblocktemplate->vTxFees[0] = -nFees;

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);

        //SPOS nBits set to 0
        if(IsStartSPosHeight(nHeight))
            pblock->nBits = 0;
        else
            pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());

        pblock->nNonce = 0;
        pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);

        if(!IsStartSPosHeight(nHeight))
        {
            CValidationState state;
            if(pindexPrev != chainActive.Tip())
            {
                LogPrintf("SPOS_Message:create new block %d is received,not generate.pindexPrev:%d\n",chainActive.Height(),pindexPrev->nHeight);
            }else if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
                throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
            }
        }
    }

    return pblocktemplate.release();
}

void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2

    //SPOS extra nonce set to zero
    if(IsStartSPosHeight(nHeight))
    {
        nExtraNonce = 0;
    }else
    {
        CMutableTransaction txCoinbase(pblock->vtx[0]);
        txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
        assert(txCoinbase.vin[0].scriptSig.size() <= 100);

        pblock->vtx[0] = txCoinbase;
        pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
    }
}

/*
 * SPOS Coinbase add version,serailze KeyID and the sign of the collateral address
*/
bool CoinBaseAddSPosExtraData(CBlock* pblock, const CBlockIndex* pindexPrev,const CMasternode& mn)
{
    unsigned int nHeight = pindexPrev->nHeight+1;
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(0)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    //1.add spos
    txCoinbase.vout[0].vReserve.push_back('s');
    txCoinbase.vout[0].vReserve.push_back('p');
    txCoinbase.vout[0].vReserve.push_back('o');
    txCoinbase.vout[0].vReserve.push_back('s');

    //2.add version
    uint16_t nSPOSVersion = SPOS_VERSION;
    const unsigned char* pVersion = (const unsigned char*)&nSPOSVersion;
    txCoinbase.vout[0].vReserve.push_back(pVersion[0]);
    txCoinbase.vout[0].vReserve.push_back(pVersion[1]);

    //3.add serialize KeyID of public key
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.reserve(1000);
    ssKey << mn.pubKeyMasternode.GetID();
    string serialPubKeyId = ssKey.str();

    for(unsigned int i = 0; i < serialPubKeyId.size(); i++)
        txCoinbase.vout[0].vReserve.push_back(serialPubKeyId[i]);

    //4.add the sign of safe+spos+version+pubkey
    string strSignMessage= "";
    for(unsigned int i=0; i< txCoinbase.vout[0].vReserve.size();i++)
        strSignMessage.push_back(txCoinbase.vout[0].vReserve[i]);
    std::vector<unsigned char> vchSig;
    if(!CMessageSigner::SignMessage(strSignMessage, vchSig, activeMasternode.keyMasternode)) {
        LogPrintf("SPOS_Error:SignMessage() failed\n");
        return false;
    }

    std::string strError;
    if(!CMessageSigner::VerifyMessage(mn.pubKeyMasternode, vchSig, strSignMessage, strError)) {
        LogPrintf("SPOS_Error:VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    for(unsigned int i=0; i< vchSig.size(); i++)
        txCoinbase.vout[0].vReserve.push_back(vchSig[i]);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);

    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

// ***TODO*** ScanHash is not yet used in Safe
//
// ScanHash scans nonces looking for a hash with at least some zero bits.
// The nonce is usually preserved between calls, but periodically or if the
// nonce is 0xffff0000 or above, the block is rebuilt and nNonce starts over at
// zero.
//
//bool static ScanHash(const CBlockHeader *pblock, uint32_t& nNonce, uint256 *phash)
//{
//    // Write the first 76 bytes of the block header to a double-SHA256 state.
//    CHash256 hasher;
//    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
//    ss << *pblock;
//    assert(ss.size() == 80);
//    hasher.Write((unsigned char*)&ss[0], 76);

//    while (true) {
//        nNonce++;

//        // Write the last 4 bytes of the block header (the nonce) to a copy of
//        // the double-SHA256 state, and compute the result.
//        CHash256(hasher).Write((unsigned char*)&nNonce, 4).Finalize((unsigned char*)phash);

//        // Return the nonce if the hash has at least some zero bits,
//        // caller will check if it has enough to reach the target
//        if (((uint16_t*)phash)[15] == 0)
//            return true;

//        // If nothing found after trying for a while, return -1
//        if ((nNonce & 0xfff) == 0)
//            return false;
//    }
//}

static bool ProcessBlockFound(const CBlock* pblock, const CChainParams& chainparams)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("ProcessBlockFound -- generated block is stale");
    }

    // Inform about the new block
    GetMainSignals().BlockFound(pblock->GetHash());

    // Process this block the same as if we had received it from another node
    if (!ProcessNewBlock(chainparams, pblock, true, NULL, NULL))
        return error("ProcessBlockFound -- ProcessNewBlock() failed, block not accepted");

    return true;
}

void static BitcoinMiner(const CChainParams& chainparams, CConnman& connman)
{
    LogPrintf("SafeMiner -- started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("safe-miner");

    unsigned int nExtraNonce = 0;

    boost::shared_ptr<CReserveScript> coinbaseScript;
    GetMainSignals().ScriptForMining(coinbaseScript);

    try {
        // Throw an error if no script was provided.  This can happen
        // due to some internal error but also if the keypool is empty.
        // In the latter case, already the pointer is NULL.
        if (!coinbaseScript || coinbaseScript->reserveScript.empty())
            throw std::runtime_error("No coinbase script available (mining requires a wallet)");

        while (true) {
            if (chainparams.MiningRequiresPeers()) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                do {
                    bool fvNodesEmpty = connman.GetNodeCount(CConnman::CONNECTIONS_ALL) == 0;
                    if (!fvNodesEmpty && !IsInitialBlockDownload() && masternodeSync.IsSynced())
                        break;
                    MilliSleep(1000);
                } while (true);
            }


            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex* pindexPrev = chainActive.Tip();
            if(!pindexPrev) break;

            //pow change to pos,then stop this thread
            if(IsStartSPosHeight(pindexPrev->nHeight+1))
                break;

            std::unique_ptr<CBlockTemplate> pblocktemplate(CreateNewBlock(chainparams, coinbaseScript->reserveScript));
            if (!pblocktemplate.get())
            {
                LogPrintf("SPOS_Warning:Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                return;
            }
            CBlock *pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            LogPrintf("SafeMiner -- Running miner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
                ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

            //
            // Search
            //
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);
            while (true)
            {
                unsigned int nHashesDone = 0;

                uint256 hash;
                while (true)
                {
                    hash = pblock->GetHash();
                    if (UintToArith256(hash) <= hashTarget)
                    {
#if SCN_CURRENT == SCN__main
                        //do nothing
#elif SCN_CURRENT == SCN__dev
                        {
                            srand((unsigned int)time(NULL));
                            //int nTime = ((rand() % GetArg("-sleep_offset", 1)) + GetArg("-sleep_min", 24)) * 1000;
                            int nTime = ((rand() % GetArg("-sleep_offset", 1)) + GetArg("-sleep_min", chainparams.GetConsensus().nPowTargetSpacing)) * 1000;
                            MilliSleep(nTime);
                        }
#elif SCN_CURRENT == SCN__test
                        {
                            srand((unsigned int)time(NULL));
                            int nTime = ((rand() % GetArg("-sleep_offset", 1)) + GetArg("-sleep_min", 4)) * 1000;
                            MilliSleep(nTime);
                        }
#else
#error unsupported <safe chain name>
#endif
                        // Found a solution
                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
                        LogPrintf("SafeMiner:\n  proof-of-work found\n  hash: %s\n  target: %s\n", hash.GetHex(), hashTarget.GetHex());
                        ProcessBlockFound(pblock, chainparams);
                        SetThreadPriority(THREAD_PRIORITY_LOWEST);
                        coinbaseScript->KeepScript();

                        // In regression test mode, stop mining after a block is found. This
                        // allows developers to controllably generate a block on demand.
                        if (chainparams.MineBlocksOnDemand())
                            throw boost::thread_interrupted();

                        break;
                    }
                    pblock->nNonce += 1;
                    nHashesDone += 1;
                    if ((pblock->nNonce & 0xFF) == 0)
                        break;
                }

                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();
                // Regtest mode doesn't require peers
                if (connman.GetNodeCount(CConnman::CONNECTIONS_ALL) == 0 && chainparams.MiningRequiresPeers())
                    break;
                //4294901760
                if (pblock->nNonce >= 0xffff0000)
                    break;
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    break;
                if (pindexPrev != chainActive.Tip())
                    break;

                // Update nTime every few seconds
                if (UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev) < 0)
                    break; // Recreate the block if the clock has run backwards,
                           // so that we can use the correct time.
                if (chainparams.GetConsensus().fPowAllowMinDifficultyBlocks)
                {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    }
    catch (const boost::thread_interrupted&)
    {
        LogPrintf("SafeMiner -- terminated\n");
        throw;
    }
    catch (const std::runtime_error &e)
    {
        LogPrintf("SafeMiner -- runtime error: %s\n", e.what());
        return;
    }
}

/*
    Consensus Use Safe Pos
*/
static void ConsensusUseSPos(const CChainParams& chainparams,CConnman& connman,CBlockIndex* pindexPrev
                             ,unsigned int nNewBlockHeight,CBlock *pblock,boost::shared_ptr<CReserveScript>& coinbaseScript
                             ,unsigned int nTransactionsUpdatedLast,int64_t& nNextTime,unsigned int& nSleepMS
                             ,int64_t& nNextLogTime,int64_t& nNextLogAllowTime,unsigned int& nWaitBlockHeight
                             ,std::vector<CMasternode>& tmpVecResultMasternodes,int nSposGeneratedIndex
                             ,int64_t& nStartNewLoopTime,unsigned int& nEmptySposCntHeight,unsigned int& nAbnormalSposCntHeight)
{
    int index = 0;
    unsigned int masternodeSPosCount = tmpVecResultMasternodes.size();
    int64_t nIntervalMS = 500;
    if(masternodeSPosCount == 0 && nEmptySposCntHeight!=nNewBlockHeight)
    {
        LogPrintf("SPOS_Error:vecMasternodes is empty,please checkout masternodelist full or config\n");
        nEmptySposCntHeight = nNewBlockHeight;
        return;
    }

    //if masternodeSPosCount less than g_nMasternodeSPosCount,still continue,just % actual masternodeSPosCount
    if(masternodeSPosCount != g_nMasternodeSPosCount && nAbnormalSposCntHeight != nNewBlockHeight)
    {
        LogPrintf("SPOS_Warning:system g_nMasternodeSPosCount:%d,curr vecMasternodes size:%d\n",g_nMasternodeSPosCount,masternodeSPosCount);
        nAbnormalSposCntHeight = nNewBlockHeight;
    }

    //1.3
    pblock->nTime = GetTime();
    int64_t nCurrTime = GetTimeMillis();
    if(nCurrTime/1000 + g_nAllowableErrorTime < (int64_t)pindexPrev->nTime)
    {
        if(nCurrTime-nNextLogAllowTime>10*1000)
        {
            string strCurrTime = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nCurrTime/1000);
            string strBlockTime = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexPrev->nTime);
            LogPrintf("SPOS_Warning:current time(%lld,%s) add allowable err time %d less than new block time(%lld,%s)\n",nCurrTime/1000,strCurrTime,g_nAllowableErrorTime,
                      pindexPrev->nTime,strBlockTime);
            nNextLogAllowTime = nCurrTime;
        }
        return;
    }

    int64_t interval = Params().GetConsensus().nSPOSTargetSpacing;
    int64_t nTimeInerval = pblock->nTime - g_nPushForwardTime + interval - nStartNewLoopTime/ 1000;
    int64_t nTimeIntervalCnt = (nTimeInerval / interval - 2);
    //to avoid nTimeIntervalCnt=masternodeSPosCount,first time nTimeIntervalCnt:-1,index:-1
    if(nTimeIntervalCnt<0)
        return;

    index = nTimeIntervalCnt % masternodeSPosCount;
    nNextTime = nStartNewLoopTime + g_nPushForwardTime*1000 + (nTimeIntervalCnt+1)*interval*1000;

    if(index<0||index>=(int)masternodeSPosCount)
    {
        LogPrintf("SPOS_Error:invalid index:%d,nTimeInterval:%d\n",index,nTimeInerval);
        return;
    }

    CMasternode& mn = tmpVecResultMasternodes[index];
    string masterIP = mn.addr.ToStringIP();
    string localIP = activeMasternode.service.ToStringIP();
    unsigned int nHeight = pindexPrev->nHeight+1;
    pblock->nNonce = mn.getCanbeSelectTime(nHeight);

    if(activeMasternode.pubKeyMasternode != mn.GetInfo().pubKeyMasternode)
    {
        if(nNewBlockHeight != nWaitBlockHeight && pblock->nTime != nNextLogTime)
        {
            LogPrintf("SPOS_Message:Wait MastnodeIP[%d]:%s to generate pos block,current block:%d.blockTime:%lld,g_nStartNewLoopTime:%lld,"
                      "local collateral address:%s,masternode collateral address:%s,nTimeInerval:%d\n",index
                      ,masterIP,pindexPrev->nHeight,pblock->nTime,nStartNewLoopTime
                      ,CBitcoinAddress(activeMasternode.pubKeyMasternode.GetID()).ToString()
                      ,CBitcoinAddress(mn.pubKeyMasternode.GetID()).ToString(),nTimeInerval);
        }
        nNextLogTime = pblock->nTime;
        nWaitBlockHeight = nNewBlockHeight;
        return;
    }

    int64_t nActualTimeMillisInterval = std::abs(nNextTime - nCurrTime);
    if(nActualTimeMillisInterval > nIntervalMS && nNextTime!=0 && nSposGeneratedIndex != -2)
    {
        if(index != nSposGeneratedIndex)
            LogPrintf("SPOS_Warning:nActualTimeMillisInterval(%d) big than nIntervalMS(%d),currblock:%d,sposIndex:%d\n"
                      ,nActualTimeMillisInterval,nIntervalMS,pindexPrev->nHeight,nSposGeneratedIndex);
        return;
    }

    //it's turn to generate block
    LogPrintf("SPOS_Info:Self mastnodeIP[%d]:%s generate pos block:%d.nActualTimeMillisInterval:%d,keyid:%s,nCurrTime:%lld,g_nStartNewLoopTime:%lld,"
              "blockTime:%lld,g_nSposIndex:%d,nTimeInerval:%d,g_nPushForwardTime:%d\n",index,localIP,nNewBlockHeight,nActualTimeMillisInterval,
              mn.pubKeyMasternode.GetID().ToString(),nCurrTime,nStartNewLoopTime,pblock->nTime,nSposGeneratedIndex,nTimeInerval,g_nPushForwardTime);

    SetThreadPriority(THREAD_PRIORITY_NORMAL);

    //coin base add extra data
    if(!CoinBaseAddSPosExtraData(pblock,pindexPrev,mn))
        return;

    if (pblock->nNonce <= g_nMasternodeCanBeSelectedTime)
    {
        LogPrintf("SPOS_Warning:the activation time of the selected master node is less than or equal to the master node "
                  "can be selected time of the limit. pblock->nNonce:%d, g_nMasternodeCanBeSelectedTime:%d\n",
                  pblock->nNonce, g_nMasternodeCanBeSelectedTime);
        return;
    }

    if(pindexPrev != chainActive.Tip())
    {
        LogPrintf("SPOS_Error:self generate block %d is received,not generate.pindexPrev:%d\n",chainActive.Height(),pindexPrev->nHeight);
        return;
    }

    CValidationState state;
    if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }

    {
        LOCK(cs_main);
        LogPrintf("SPOS_Message:test self block validate success\n");       
        {
            LOCK(cs_spos);
            g_nSposGeneratedIndex = index;
        }
        ProcessBlockFound(pblock, chainparams);

        SetThreadPriority(THREAD_PRIORITY_LOWEST);
        coinbaseScript->KeepScript();

        if(masternodeSPosCount==1)//XJTODO
            nSleepMS = Params().GetConsensus().nSPOSTargetSpacing*1000;
        else
            nSleepMS = nIntervalMS;
    }

    // In regression test mode, stop mining after a block is found. This
    // allows developers to controllably generate a block on demand.

    if (chainparams.MineBlocksOnDemand())
    {
        LogPrintf("SPOS_Warning:MineBlocksOnDemand\n");
        throw boost::thread_interrupted();
    }

    // Check for stop or if block needs to be rebuilt
    boost::this_thread::interruption_point();
    // Regtest mode doesn't require peers
    if (connman.GetNodeCount(CConnman::CONNECTIONS_ALL) == 0 && chainparams.MiningRequiresPeers())
    {
        LogPrintf("SPOS_Warning:GetNodeCount fail\n");
        return;
    }
//    if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast /*&& GetTime() - nCurrTime > 60*/)
//    {
//        LogPrintf("SPOS_Warning:mempool fail,mempool.GetTransactionsUpdated():%d,nTransactionsUpdatedLast:%d\n"
//                  ,mempool.GetTransactionsUpdated(),nTransactionsUpdatedLast);
//        return;
//    }
    if (pindexPrev != chainActive.Tip())
    {
        //LogPrintf("SPOS_Warning:tip not equal\n");
        return;
    }

    // Update nTime every few seconds
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    LogPrintf("SPOS_Message:generate block finished\n");
}

// ***TODO*** that part changed in bitcoin, we are using a mix with old one here for now
void static SposMiner(const CChainParams& chainparams, CConnman& connman)
{
    LogPrintf("SPOS_Message:SafeSposMiner is -- started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("safe-spos-miner");

    unsigned int nExtraNonce = 0;

    boost::shared_ptr<CReserveScript> coinbaseScript;
    GetMainSignals().ScriptForMining(coinbaseScript);

    try {
        // Throw an error if no script was provided.  This can happen
        // due to some internal error but also if the keypool is empty.
        // In the latter case, already the pointer is NULL.
        if (!coinbaseScript || coinbaseScript->reserveScript.empty())
            throw std::runtime_error("No coinbase script available (mining requires a wallet)");

        g_nStartNewLoopTimeMS = 0;
        {
            LOCK(cs_spos);
            g_nStartNewLoopTimeMS = GetTime()*1000;
        }
        unsigned int nWaitBlockHeight = 0,nEmptySposCntHeight = 0,nAbnormalSposCntHeight = 0;
        int64_t nNextBlockTime = 0,nNextLogTime = 0,nLogOutput = 0,nLastMasternodeCount = 0,nNextLogAllowTime = 0;
        while (true) {
            if (chainparams.MiningRequiresPeers()) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                do {
                    bool fvNodesEmpty = connman.GetNodeCount(CConnman::CONNECTIONS_ALL) == 0;
                    if (!fvNodesEmpty && !IsInitialBlockDownload() && masternodeSync.IsSynced())
                        break;
                    MilliSleep(50);
                } while (true);
            }

            unsigned int nSleepMS = 0;
            CBlockIndex* pindexPrev = chainActive.Tip();
            if(!pindexPrev)
            {
                LogPrintf("SPOS_Warning:SposMiner pindexPrev is NULL,size:%d\n",chainActive.Height());
                break;
            }
            unsigned int nNewBlockHeight = chainActive.Height() + 1;
            if(IsStartSPosHeight(nNewBlockHeight))
            {
                // Create new block
                unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
                if(activeMasternode.outpoint.IsNull())
                {
                    if(nLogOutput==0)
                    {
                        LogPrintf("SPOS_Warning:self masternode outpoint is empty,if self is masternode maybe need to wait sync or reindex or start alias\n");
                        nLogOutput = 1;
                    }
                    continue;
                }

                if(nLogOutput==1)
                {
                    nLogOutput = 0;
                    LogPrintf("SPOS_Warning:self masternode empty outpoint is normal,start miner\n");
                }

                std::unique_ptr<CBlockTemplate> pblocktemplate(CreateNewBlock(chainparams, coinbaseScript->reserveScript));
                if (!pblocktemplate.get())
                {
                    LogPrintf("SafeSposMiner -- Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                    return;
                }

                if(pindexPrev != chainActive.Tip())
                {
                    LogPrintf("SPOS_Message:create new block(%d) fail,already recived the block:%d\n",nNewBlockHeight,chainActive.Height());
                    continue;
                }

                CBlock *pblock = &pblocktemplate->block;
                IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

//                LogPrintf("SPOS_Message:Running miner with %u transactions in block (%u bytes),currHeight:%d\n",pblock->vtx.size(),
//                          ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION),pindexPrev->nHeight);

                std::vector<CMasternode> tmpVecResultMasternodes;
                int nSposGeneratedIndex=0,masternodeSPosCount=0;
                int64_t nStartNewLoopTime=0;
                {
                    LOCK(cs_spos);
                    for(auto& mn:g_vecResultMasternodes)
                    {
                        tmpVecResultMasternodes.push_back(mn);
                        masternodeSPosCount++;
                    }
                    nStartNewLoopTime = g_nStartNewLoopTimeMS;
                    nSposGeneratedIndex = g_nSposGeneratedIndex;
                }
                if(masternodeSPosCount != 0)
                {
                    ConsensusUseSPos(chainparams,connman,pindexPrev,nNewBlockHeight,pblock,coinbaseScript,nTransactionsUpdatedLast,nNextBlockTime,
                                     nSleepMS,nNextLogTime,nNextLogAllowTime,nWaitBlockHeight,tmpVecResultMasternodes,nSposGeneratedIndex,
                                     nStartNewLoopTime,nEmptySposCntHeight,nAbnormalSposCntHeight);
                }else if(nLastMasternodeCount != 0)
                {
                    LogPrintf("SPOS_Error:vec_masternodes is empty,nLastMasternodeCount:%d\n",nLastMasternodeCount);
                }
                nLastMasternodeCount = masternodeSPosCount;
            }
            if(nSleepMS>0)
                MilliSleep(nSleepMS);
            else
                MilliSleep(50);
        }
    }
    catch (const boost::thread_interrupted&)
    {
        LogPrintf("SPOS_Warning:SafeMiner -- terminated\n");
        throw;
    }
    catch (const std::runtime_error &e)
    {
        LogPrintf("SPOS_Warning:SafeMiner -- runtime error: %s\n", e.what());
        return;
    }
    LogPrintf("SPOS_Warning:spos miner thread is exit\n");
}

void GenerateBitcoins(bool fGenerate, int nThreads, const CChainParams& chainparams, CConnman& connman)
{
#if SCN_CURRENT == SCN__main
    //do nothing
#elif SCN_CURRENT == SCN__dev || SCN_CURRENT == SCN__test
    if(!GetBoolArg("-lmb_gen", false))
        return;
#else
#error unsupported <safe chain name>
#endif

    static boost::thread_group* minerThreads = NULL;

    if (nThreads < 0)
        nThreads = GetNumCores();

    if (minerThreads != NULL)
    {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        minerThreads->create_thread(boost::bind(&BitcoinMiner, boost::cref(chainparams), boost::ref(connman)));
}

void GenerateBitcoinsBySPOS(bool fGenerate, int nThreads, const CChainParams& chainparams, CConnman& connman)
{
    if(fGenerate)
    {
        if (!activeMasternode.pubKeyMasternode.IsValid())
        {
            LogPrintf("SPOS_Warning:only the master node needs to open SPOS mining\n");
            return;
        }

        if((g_nStartSPOSHeight-1)%g_nMasternodeSPosCount!=0)
            LogPrintf("SPOS_Warning:invalid spos height or spos count config\n");

        LogPrintf("SPOS_Message:GenerateBitcoinsBySPOS,start_spos_height:%d,masternode_spos_count:%d,masternode_can_be_selected_time:%d\n"
                  , g_nStartSPOSHeight,g_nMasternodeSPosCount,g_nMasternodeCanBeSelectedTime);
    }

    static boost::thread_group* sposMinerThreads = NULL;

    if (nThreads < 0)
        nThreads = GetNumCores();

    if (sposMinerThreads != NULL)
    {
        sposMinerThreads->interrupt_all();
        delete sposMinerThreads;
        sposMinerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    sposMinerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        sposMinerThreads->create_thread(boost::bind(&SposMiner, boost::cref(chainparams), boost::ref(connman)));
}

void ThreadSPOSAutoReselect(const CChainParams& chainparams, CConnman& connman)
{
    LogPrintf("SPOS_Message:SPOSAutoReselectThread is -- started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("spos-autoreselect");

    try {
        int nTmpTimeoutCount = -1;
        int nLastTimeoutHeight = 0;
        while (true)
        {
            boost::this_thread::interruption_point();
            if (chainparams.MiningRequiresPeers())
            {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                do {
                    boost::this_thread::interruption_point();
                    bool fvNodesEmpty = connman.GetNodeCount(CConnman::CONNECTIONS_ALL) == 0;
                    if (!fvNodesEmpty && !IsInitialBlockDownload() && masternodeSync.IsSynced() && !g_fReceiveBlock)
                        break;
                    MilliSleep(50);
                } while (true);
            }

            CBlockIndex* pindexPrev = chainActive.Tip();
            if(!pindexPrev)
            {
                LogPrintf("SPOS_Warning:ThreadSPOSAutoReselect pindexPrev is NULL,size:%d\n",chainActive.Height());
                MilliSleep(nSposSleeptime);
                continue;
            }
            int nCurrBlockHeight = chainActive.Height();
            if(!IsStartSPosHeight(nCurrBlockHeight))
            {
                MilliSleep(nSposSleeptime);
                continue;
            }

            int nTimeout = g_nMinerBlockTimeout;
            uint32_t nCurrTime = GetTime();
            int nTimeoutRet = nCurrTime - pindexPrev->GetBlockTime();
            if(nTimeoutRet <= nTimeout)
            {
                UpdateGlobalTimeoutCount(0);
                MilliSleep(nSposSleeptime);
                continue;
            }
            int nTimeoutCount = nTimeoutRet/g_nMinerBlockTimeout;
            if(nTimeoutCount <= g_nTimeoutCount && nLastTimeoutHeight==nCurrBlockHeight)
            {
                if(nTimeoutCount != nTmpTimeoutCount)
                {
                    LogPrintf("SPOS_Warning:timeout reselect masternode,but the timeInterval is %d,need to wait a few seconds,nTimeoutCount:%d,g_nTimeoutCount:%d\n",
                              nTimeoutRet,nTimeoutCount,g_nTimeoutCount);
                }
                nTmpTimeoutCount = nTimeoutCount;
                MilliSleep(nSposSleeptime);
                continue;
            }
            nLastTimeoutHeight = nCurrBlockHeight;
            UpdateGlobalTimeoutCount(nTimeoutCount);
            int nForwardHeight = 0,nScoreHeight = 0;
            updateForwardHeightAndScoreHeight(nCurrBlockHeight,nForwardHeight,nScoreHeight);
            LogPrintf("SPOS_Warning:timeout reselect masternode,nTimeoutRet:%d bigger than nTimeout:%d,currTime:%d,g_nTimeoutCount:%d,"
                      "heightIndex:%d,nScoreHeight:%d\n",nTimeoutRet,nTimeout,nCurrTime,g_nTimeoutCount,nForwardHeight,nScoreHeight);
            CBlockIndex* scoreIndex = chainActive[nScoreHeight];
            if(scoreIndex==NULL)
            {
                LogPrintf("SPOS_Warning:scoreIndex is NULL,height:%d,chainActive size:%d,reselect loop fail\n",nScoreHeight,chainActive.Height());
                MilliSleep(nSposSleeptime);
                continue;
            }
            CBlockIndex* forwardIndex = chainActive[nForwardHeight];
            if(forwardIndex==NULL)
            {
                LogPrintf("SPOS_Warning:forwardIndex is NULL,height:%d,chainActive size:%d,reselect loop fail\n",nForwardHeight,chainActive.Height());
                MilliSleep(nSposSleeptime);
                continue;
            }
            std::vector<CMasternode> tmpVecResultMasternodes;
            bool bClearVec=false;
            int nSelectMasterNodeRet=g_nSelectGlobalDefaultValue,nSposGeneratedIndex=g_nSelectGlobalDefaultValue;
            int64_t nStartNewLoopTime=g_nSelectGlobalDefaultValue;
            bool fOverTimeoutLimit = g_nTimeoutCount >= g_nMaxTimeoutCount;
            bool fReselect = true;
            SPORK_SELECT_LOOP nSporkSelectLoop = NO_SPORK_SELECT_LOOP;
            if (sporkManager.IsSporkActive(SPORK_6_SPOS_ENABLED) && !fOverTimeoutLimit)
            {
                int64_t ntempSporkValue = sporkManager.GetSporkValue(SPORK_6_SPOS_ENABLED);
                std::string strSporkValue = boost::lexical_cast<std::string>(ntempSporkValue);
                std::string strHeight = strSporkValue.substr(0, strSporkValue.length() - 1);
                std::string strOfficialMasterNodeCount = strSporkValue.substr(strSporkValue.length() - 1);
                int nHeight = boost::lexical_cast<int>(strHeight);
                int nOfficialMasterNodeCount = boost::lexical_cast<int>(strOfficialMasterNodeCount);
                LogPrintf("SPOS_Message: ThreadSPOSAutoReselect() strHeight:%s---strOfficialMasterNodeCount:%s---nHeight:%d--nOfficialMasterNodeCount:%d--nNewBlockHeight:%d\n",
                          strHeight, strOfficialMasterNodeCount, nHeight, nOfficialMasterNodeCount, nCurrBlockHeight);

                if (nCurrBlockHeight + 1 >= nHeight)
                {
                    fReselect = false;
                    if (nOfficialMasterNodeCount <= 0 || nOfficialMasterNodeCount > g_nMasternodeSPosCount)
                    {
                        LogPrintf("SPOS_Warning: ThreadSPOSAutoReselect() nOfficialMasterNodeCount is error,nNewBlockHeight:%d, nOfficialMasterNodeCount:%d, g_nMasternodeSPosCount:%d\n",nCurrBlockHeight, nOfficialMasterNodeCount, g_nMasternodeSPosCount);
                        MilliSleep(nSposSleeptime);
                        continue;
                    }

                    nSporkSelectLoop = SPORK_SELECT_LOOP_1;
                    SelectMasterNodeByPayee(nCurrBlockHeight,forwardIndex->nTime,scoreIndex->nTime,true,true,tmpVecResultMasternodes,bClearVec,
                                nSelectMasterNodeRet,nSposGeneratedIndex,nStartNewLoopTime,true, nOfficialMasterNodeCount, nSporkSelectLoop, false);

                    nSporkSelectLoop = SPORK_SELECT_LOOP_2;
                    if (g_nMasternodeSPosCount - nOfficialMasterNodeCount > 0 && nSelectMasterNodeRet > 0)
                        SelectMasterNodeByPayee(nCurrBlockHeight,forwardIndex->nTime,scoreIndex->nTime,false,true,tmpVecResultMasternodes,bClearVec,
                                                nSelectMasterNodeRet,nSposGeneratedIndex,nStartNewLoopTime,true, g_nMasternodeSPosCount - nOfficialMasterNodeCount, nSporkSelectLoop, true);
                }
            }

            if(fReselect)
            {
                if(fOverTimeoutLimit)
                    nSporkSelectLoop = SPORK_SELECT_LOOP_OVER_TIMEOUT_LIMIT;
                SelectMasterNodeByPayee(nCurrBlockHeight,forwardIndex->nTime,scoreIndex->nTime,fOverTimeoutLimit,true,tmpVecResultMasternodes,bClearVec,
                                        nSelectMasterNodeRet,nSposGeneratedIndex,nStartNewLoopTime,true, g_nMasternodeSPosCount, nSporkSelectLoop, false);
            }
            UpdateMasternodeGlobalData(tmpVecResultMasternodes,bClearVec,nSelectMasterNodeRet,nSposGeneratedIndex,nStartNewLoopTime);

            MilliSleep(nSposSleeptime);
        }
    }
    catch (const boost::thread_interrupted&)
    {
        LogPrintf("SPOS_Warning:SPOSAutoReselect -- terminated\n");
        throw;
    }
    catch (const std::runtime_error &e)
    {
        LogPrintf("SPOS_Warning:SPOSAutoReselect -- runtime error: %s\n", e.what());
        return;
    }
    LogPrintf("SPOS_Warning:spos auto reselect thread is exit\n");
}

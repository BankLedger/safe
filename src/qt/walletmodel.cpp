// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2018-2019 The Safe Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "walletmodel.h"
#include "walletview.h"

#include "addresstablemodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "paymentserver.h"
#include "recentrequeststablemodel.h"
#include "transactiontablemodel.h"
#include "lockedtransactiontablemodel.h"
#include "candytablemodel.h"
#include "assetsdistributerecordmodel.h"
#include "applicationsregistrecordmodel.h"
#include "masternode-sync.h"

#include "base58.h"
#include "keystore.h"
#include "validation.h"
#include "net.h" // for g_connman
#include "sync.h"
#include "ui_interface.h"
#include "util.h" // for GetBoolArg
#include "wallet/wallet.h"
#include "wallet/walletdb.h" // for BackupWallet
#include "main.h"

#include "instantx.h"
#include "spork.h"
#include "privatesend-client.h"
#include "transactionrecord.h"
#include "init.h"
#include "utilmoneystr.h"
#include <QMessageBox>
#include <stdint.h>

#include <QDebug>
#include <QSet>
#include <QTimer>
#include <QApplication>
#include <QThread>
#include <boost/foreach.hpp>
#include <boost/thread.hpp>

extern boost::thread_group* g_threadGroup;

WalletModel::WalletModel(const PlatformStyle *platformStyle, CWallet *wallet, OptionsModel *optionsModel, QObject *parent) :
    QObject(parent), wallet(wallet), optionsModel(optionsModel), addressTableModel(0),
    transactionTableModel(0),
    lockedTransactionTableModel(0),
    candyTableModel(0),
    assetsDistributeTableModel(0),
    applicationsRegistTableModel(0),
    recentRequestsTableModel(0),
    cachedBalance(0),
    cachedUnconfirmedBalance(0),
    cachedImmatureBalance(0),
    cachedLockedBalance(0),
    cachedAnonymizedBalance(0),
    cachedWatchOnlyBalance(0),
    cachedWatchUnconfBalance(0),
    cachedWatchImmatureBalance(0),
    cachedWatchLockedBalance(0),
    cachedEncryptionStatus(Unencrypted),
    cachedNumBlocks(0),
    cachedTxLocks(0),
    cachedPrivateSendRounds(0),
    nCheckIncrease(0)
{
    fHaveWatchOnly = wallet->HaveWatchOnly();
    fForceCheckBalanceChanged = false;
	pWalletView = 0;

    addressTableModel = new AddressTableModel(wallet, this);
    transactionTableModel = new TransactionTableModel(platformStyle, wallet, SHOW_TX, this);
    lockedTransactionTableModel = new LockedTransactionTableModel(platformStyle, wallet,SHOW_LOCKED_TX, this);
    candyTableModel = new CandyTableModel(platformStyle, wallet, SHOW_CANDY_TX,this);
    assetsDistributeTableModel = new AssetsDistributeRecordModel(platformStyle,wallet,SHOW_ASSETS_DISTRIBUTE,this);
    applicationsRegistTableModel = new ApplicationsRegistRecordModel(platformStyle,wallet,SHOW_APPLICATION_REGIST,this);
    recentRequestsTableModel = new RecentRequestsTableModel(wallet, this);

    subscribeToCoreSignals();

	pTimer = new QTimer(this);
	connect(pTimer, SIGNAL(timeout()), this, SLOT(refreshTransaction_slot()));
	
	qRegisterMetaType<QList<TransactionRecord> >("QList<TransactionRecord>");
	qRegisterMetaType<uint256>("uint256");
	qRegisterMetaType<QMap<uint256, QList<TransactionRecord>> >("QMap<uint256, QList<TransactionRecord>>");
	qRegisterMetaType<QMap<uint256, NewTxData> >("QMap<uint256, NewTxData>");	

	pUpdateTransaction = new CUpdateTransaction();
	connect(pUpdateTransaction, 
		SIGNAL(updateAllTransaction(const QMap<uint256, QList<TransactionRecord> >, const QMap<uint256, NewTxData>)),
		this, 
		SLOT(updateAllTransaction_slot(const QMap<uint256, QList<TransactionRecord> >, const QMap<uint256, NewTxData>)));
	pUpdateTransaction->init(this, wallet);
}

WalletModel::~WalletModel()
{
	pTimer->stop();
	disconnect(pTimer, SIGNAL(timeout()), this, SLOT(refresh_slot()));
	
	disconnect(pUpdateTransaction,
		SIGNAL(updateAllTransaction(const QMap<uint256, QList<TransactionRecord> >, const QMap<uint256, NewTxData>)),
		this,
		SLOT(updateAllTransaction_slot(const QMap<uint256, QList<TransactionRecord> >, const QMap<uint256, NewTxData>)));
	
	if (pUpdateTransaction != NULL)
	{
		delete pUpdateTransaction;
		pUpdateTransaction = NULL;
	}

    unsubscribeFromCoreSignals();
}

CAmount WalletModel::getBalance(const CCoinControl *coinControl, const bool fAsset, const uint256 *pAssetId, const CBitcoinAddress *pAddress,bool bLock) const
{
    if (coinControl)
    {
        CAmount nBalance = 0;
        std::vector<COutput> vCoins;
        wallet->AvailableCoins(vCoins, true, coinControl);
        BOOST_FOREACH(const COutput& out, vCoins)
            if(out.fSpendable)
                nBalance += out.tx->vout[out.i].nValue;

        return nBalance;
    }

    return wallet->GetBalance(fAsset,pAssetId,pAddress,bLock);
}


CAmount WalletModel::getAnonymizedBalance(bool bLock) const
{
    return wallet->GetAnonymizedBalance(bLock);
}

CAmount WalletModel::getUnconfirmedBalance(const bool fAsset, const uint256 *pAssetId, const CBitcoinAddress *pAddress,bool bLock) const
{
    return wallet->GetUnconfirmedBalance(fAsset,pAssetId,pAddress,bLock);
}

CAmount WalletModel::getImmatureBalance(const bool fAsset, const uint256 *pAssetId, const CBitcoinAddress *pAddress,bool bLock) const
{
    return wallet->GetImmatureBalance(fAsset,pAssetId,pAddress,bLock);
}

CAmount WalletModel::getLockedBalance(const bool fAsset, const uint256 *pAssetId, const CBitcoinAddress *pAddress,bool bLock) const
{
    return wallet->GetLockedBalance(fAsset,pAssetId,pAddress,bLock);
}

bool WalletModel::haveWatchOnly() const
{
    return fHaveWatchOnly;
}

CAmount WalletModel::getWatchBalance(const bool fAsset, const uint256 *pAssetId, const CBitcoinAddress *pAddress,bool bLock) const
{
    return wallet->GetWatchOnlyBalance(fAsset,pAssetId,pAddress,bLock);
}

CAmount WalletModel::getWatchUnconfirmedBalance(const bool fAsset, const uint256 *pAssetId, const CBitcoinAddress *pAddress,bool bLock) const
{
    return wallet->GetUnconfirmedWatchOnlyBalance(fAsset,pAssetId,pAddress,bLock);
}

CAmount WalletModel::getWatchImmatureBalance(const bool fAsset, const uint256 *pAssetId, const CBitcoinAddress *pAddress,bool bLock) const
{
    return wallet->GetImmatureWatchOnlyBalance(fAsset,pAssetId,pAddress,bLock);
}

CAmount WalletModel::getWatchLockedBalance(const bool fAsset, const uint256 *pAssetId, const CBitcoinAddress *pAddress,bool bLock) const
{
    return wallet->GetLockedWatchOnlyBalance(fAsset,pAssetId,pAddress,bLock);
}

void WalletModel::updateStatus()
{
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if(cachedEncryptionStatus != newEncryptionStatus)
        Q_EMIT encryptionStatusChanged(newEncryptionStatus);
}

void WalletModel::updateAllBalanceChanged(bool checkIncrease)
{
    bool fPrivateSendRounds = privateSendClient.nPrivateSendRounds != cachedPrivateSendRounds;
    if(fForceCheckBalanceChanged || fPrivateSendRounds || cachedTxLocks != nCompleteTXLocks)
    {
        if(nCheckIncrease%5==0||fForceCheckBalanceChanged)
        {
            checkIncrease = false;
            nCheckIncrease = 1;
        }
		else
        {
            nCheckIncrease++;
        }

		// Balance and number of transactions might have changed
		
		cachedPrivateSendRounds = privateSendClient.nPrivateSendRounds;
		fForceCheckBalanceChanged = false;
		
		checkBalanceChanged(checkIncrease);
    }
}

void WalletModel::updateConfirmations()
{
	bool fCachedNumBlocks = chainActive.Height() != cachedNumBlocks;

	if (fCachedNumBlocks)
	{
		cachedNumBlocks = chainActive.Height();

		WalletModel::PageType pageType = WalletModel::NonePage;
		if (pWalletView)
		{
			pageType = (WalletModel::PageType)pWalletView->getPageType();
		}

		if (transactionTableModel && pageType == WalletModel::TransactionPage)
			transactionTableModel->updateConfirmations();

		if (lockedTransactionTableModel && pageType == WalletModel::LockPage)
			lockedTransactionTableModel->updateConfirmations();

		if (candyTableModel && pageType == WalletModel::CandyPage)
			candyTableModel->updateConfirmations();

		if (assetsDistributeTableModel && pageType == WalletModel::AssetPage)
			assetsDistributeTableModel->updateConfirmations();

		if (applicationsRegistTableModel && pageType == WalletModel::AppPage)
			applicationsRegistTableModel->updateConfirmations();
	}
}

void WalletModel::pollBalanceChanged(bool checkIncrease)
{
    // Get required locks upfront. This avoids the GUI from getting stuck on
    // periodical polls if the core is holding the locks for a longer time -
    // for example, during a wallet rescan.
	bool fPrivateSendRounds = privateSendClient.nPrivateSendRounds != cachedPrivateSendRounds;
	if (fForceCheckBalanceChanged || fPrivateSendRounds || cachedTxLocks != nCompleteTXLocks)
	{
		TRY_LOCK(cs_main, lockMain);
		if (!lockMain)
			return;
		TRY_LOCK(wallet->cs_wallet, lockWallet);
		if (!lockWallet)
			return;

		updateAllBalanceChanged(checkIncrease);
	}

	updateConfirmations();
}

void WalletModel::checkBalanceChanged(bool checkIncrease)
{
    if(!wallet->mapWallet_tmp.empty())
    {
        wallet->mapWallet_tmp.clear();
        std::map<uint256, CWalletTx>().swap(wallet->mapWallet_tmp);
    }

    {
        LOCK2(cs_main, wallet->cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = wallet->mapWallet.begin(); it != wallet->mapWallet.end(); ++it)
        {
            if(wallet->mapWallet_bk.count((*it).first)==0)
            {
                wallet->mapWallet_bk[(*it).first] = 1;
                wallet->mapWallet_tmp[(*it).first] = (*it).second;
            }
        }
    }

    if(checkIncrease)
    {
        //no update,return
        if(wallet->mapWallet_tmp.empty())
            return;
    }
    CAmount newLockedBalance = 0;
    CAmount newWatchLockedBalance = 0;
    newLockedBalance = getLockedBalance(false,NULL,NULL,!checkIncrease);
    newWatchLockedBalance = getWatchLockedBalance(false,NULL,NULL,!checkIncrease);
    if(checkIncrease)
    {
        if(0 != newLockedBalance || 0 != newWatchLockedBalance)
            wallet->MarkDirty();
    }else if(cachedLockedBalance !=newLockedBalance || cachedWatchLockedBalance != newWatchLockedBalance)
    {
        wallet->MarkDirty();
    }
    CAmount newBalance = getBalance(NULL,false,NULL,NULL,!checkIncrease);
    CAmount newUnconfirmedBalance = getUnconfirmedBalance(false,NULL,NULL,!checkIncrease);
    CAmount newImmatureBalance = getImmatureBalance(false,NULL,NULL,!checkIncrease);

    CAmount newAnonymizedBalance = getAnonymizedBalance();
    CAmount newWatchOnlyBalance = 0;
    CAmount newWatchUnconfBalance = 0;
    CAmount newWatchImmatureBalance = 0;

    if (haveWatchOnly())
    {
        newWatchOnlyBalance = getWatchBalance();
        newWatchUnconfBalance = getWatchUnconfirmedBalance();
        newWatchImmatureBalance = getWatchImmatureBalance();
    }

    if(checkIncrease)
    {
        if(0 != newBalance || 0 != newUnconfirmedBalance || 0 != newImmatureBalance || 0 != newLockedBalance ||
            0 != newAnonymizedBalance || 0 != nCompleteTXLocks ||
            0 != newWatchOnlyBalance || 0 != newWatchUnconfBalance || 0 != newWatchImmatureBalance || 0 != newWatchLockedBalance)
        {
            cachedBalance += newBalance;
            cachedUnconfirmedBalance += newUnconfirmedBalance;
            cachedImmatureBalance += newImmatureBalance;
            cachedLockedBalance += newLockedBalance;
            cachedAnonymizedBalance += newAnonymizedBalance;
            cachedTxLocks += nCompleteTXLocks;
            cachedWatchOnlyBalance += newWatchOnlyBalance;
            cachedWatchUnconfBalance += newWatchUnconfBalance;
            cachedWatchImmatureBalance += newWatchImmatureBalance;
            cachedWatchLockedBalance += newWatchLockedBalance;
            Q_EMIT balanceChanged(cachedBalance, cachedUnconfirmedBalance, cachedImmatureBalance, cachedLockedBalance, cachedAnonymizedBalance,
                                cachedWatchOnlyBalance, cachedWatchUnconfBalance, cachedWatchImmatureBalance, cachedWatchLockedBalance);
        }
    }else
    {
        if(cachedBalance != newBalance || cachedUnconfirmedBalance != newUnconfirmedBalance || cachedImmatureBalance != newImmatureBalance || cachedLockedBalance != newLockedBalance ||
            cachedAnonymizedBalance != newAnonymizedBalance || cachedTxLocks != nCompleteTXLocks ||
            cachedWatchOnlyBalance != newWatchOnlyBalance || cachedWatchUnconfBalance != newWatchUnconfBalance || cachedWatchImmatureBalance != newWatchImmatureBalance || cachedWatchLockedBalance != newWatchLockedBalance)
        {
            cachedBalance = newBalance;
            cachedUnconfirmedBalance = newUnconfirmedBalance;
            cachedImmatureBalance = newImmatureBalance;
            cachedLockedBalance = newLockedBalance;
            cachedAnonymizedBalance = newAnonymizedBalance;
            cachedTxLocks = nCompleteTXLocks;
            cachedWatchOnlyBalance = newWatchOnlyBalance;
            cachedWatchUnconfBalance = newWatchUnconfBalance;
            cachedWatchImmatureBalance = newWatchImmatureBalance;
            cachedWatchLockedBalance = newWatchLockedBalance;
            Q_EMIT balanceChanged(newBalance, newUnconfirmedBalance, newImmatureBalance, newLockedBalance, newAnonymizedBalance,
                                newWatchOnlyBalance, newWatchUnconfBalance, newWatchImmatureBalance, newWatchLockedBalance);
        }
    }
}

void WalletModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void WalletModel::updateAddressBook(const QString &address, const QString &label,
        bool isMine, const QString &purpose, int status)
{
    if(addressTableModel)
        addressTableModel->updateEntry(address, label, isMine, purpose, status);
}

void WalletModel::updateWatchOnlyFlag(bool fHaveWatchonly)
{
    fHaveWatchOnly = fHaveWatchonly;
    Q_EMIT notifyWatchonlyChanged(fHaveWatchonly);
}

bool WalletModel::validateAddress(const QString &address)
{
    CBitcoinAddress addressParsed(address.toStdString());
    return addressParsed.IsValid();
}

WalletModel::SendCoinsReturn WalletModel::prepareTransaction(WalletModelTransaction &transaction, const CCoinControl *coinControl, bool fAssets, const QString &assetsName)
{
    CAmount total = 0;
    bool fSubtractFeeFromAmount = false;
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> vecSend;

    if(recipients.empty())
    {
        return OK;
    }

    // This should never really happen, yet another safety check, just in case.
    if(wallet->IsLocked()) {
        return TransactionCreationFailed;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;

    // Pre-check input data for validity
    CCommonData transferData;
    Q_FOREACH(const SendCoinsRecipient &rcp, recipients)
    {
        if (rcp.fSubtractFeeFromAmount)
            fSubtractFeeFromAmount = true;

        if (rcp.paymentRequest.IsInitialized()&&!fAssets)
        {   // PaymentRequest...
            CAmount subtotal = 0;
            const payments::PaymentDetails& details = rcp.paymentRequest.getDetails();
            for (int i = 0; i < details.outputs_size(); i++)
            {
                const payments::Output& out = details.outputs(i);
                if (out.amount() <= 0) continue;
                subtotal += out.amount();
                const unsigned char* scriptStr = (const unsigned char*)out.script().data();
                CScript scriptPubKey(scriptStr, scriptStr+out.script().size());
                CAmount nAmount = out.amount();
                CRecipient recipient = {scriptPubKey, nAmount, 0, rcp.fSubtractFeeFromAmount,fAssets,rcp.strMemo.toStdString()};
                vecSend.push_back(recipient);
            }
            if (subtotal <= 0)
            {
                return InvalidAmount;
            }
            total += subtotal;
        }
        else
        {
            if(fAssets)
            {
                CBitcoinAddress assetsAddress(rcp.address.toStdString());
                if(!assetsAddress.IsValid())
                    return SendCoinsReturn(InvalidAssetRecvAddress);
                uint256 assetId;
                if(!GetAssetIdByAssetName(assetsName.toStdString(),assetId, false))
                    return SendCoinsReturn(AssetIdFail);
                if(assetId.IsNull())
                    return SendCoinsReturn(InvalidAssetId);

                CAssetId_AssetInfo_IndexValue assetInfo;
                if(!GetAssetInfoByAssetId(assetId,assetInfo, false))
                    return SendCoinsReturn(NonExistAssetId);

                CAmount nAmount = 0;
                if(!ParseFixedPoint(rcp.strAssetAmount.toStdString(), assetInfo.assetData.nDecimals, &nAmount))
                    return SendCoinsReturn(InvalidAssetAmount);
                if (!AssetsRange(nAmount))
                    return SendCoinsReturn(AmountOutOfRange);

                int nLockedMonth = rcp.nLockedMonth;
                if(nLockedMonth != 0 && !IsLockedMonthRange(nLockedMonth))
                    return SendCoinsReturn(InvalidLockedMonth);
                std::string strRemarks = rcp.strMemo.toStdString();
                if(strRemarks.size() > MAX_REMARKS_SIZE)
                    return SendCoinsReturn(InvalidRemarks);

                transferData.assetId = assetId;
                transferData.nAmount = nAmount;
                transferData.strRemarks = strRemarks;
                if (wallet->IsLocked())
                    return SendCoinsReturn(WalletLocked);

                if (wallet->GetBroadcastTransactions() && !g_connman)
                    return SendCoinsReturn(P2PMissed);

                if(wallet->GetBalance() <= 0)
                    return SendCoinsReturn(InsufficientSafeFunds);

                CAmount assetAvailableAmount= wallet->GetBalance(true, &assetId);
                if(assetAvailableAmount < nAmount)
                    return SendCoinsReturn(InsufficientAssetFunds);

                CRecipient recvRecipient = {GetScriptForDestination(assetsAddress.Get()), nAmount, nLockedMonth, false, true,rcp.strMemo.toStdString()};
                vecSend.push_back(recvRecipient);

                total += nAmount;
                 if(total > assetAvailableAmount)
                     return AmountExceedsBalance;

                 setAddress.insert(rcp.address);
                 ++nAddresses;
            }
            else
            {
                // User-entered safe address / amount:
                if(!validateAddress(rcp.address))
                {
                    return InvalidAddress;
                }
                if(rcp.amount <= 0)
                {
                    return InvalidAmount;
                }
                setAddress.insert(rcp.address);
                ++nAddresses;

                CScript scriptPubKey = GetScriptForDestination(CBitcoinAddress(rcp.address.toStdString()).Get());
                CRecipient recipient = {scriptPubKey, rcp.amount, rcp.nLockedMonth, rcp.fSubtractFeeFromAmount,fAssets,rcp.strMemo.toStdString()};
                vecSend.push_back(recipient);

                total += rcp.amount;
            }
        }
    }
    if(setAddress.size() != nAddresses)
    {
        return DuplicateAddress;
    }

    CAmount nBalance = getBalance(coinControl);

    if(!fAssets)
    {
        if(total > nBalance)
        {
            return AmountExceedsBalance;
        }
    }

    {
        LOCK2(cs_main, wallet->cs_wallet);

        transaction.newPossibleKeyChange(wallet);

        CAmount nFeeRequired = 0;
        int nChangePosRet = -1;
        std::string strFailReason;

        CWalletTx *newTx = transaction.getTransaction();
        CReserveKey *keyChange = transaction.getPossibleKeyChange();

        if(recipients[0].fUseInstantSend && total > sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)*COIN){
            Q_EMIT message(tr("Send Coins"), tr("InstantSend doesn't support sending high values of transaction inputs yet. Values of transaction inputs are currently limited to %1 SAFE.").arg(sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)),
                         CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        bool fCreated = false;
        if(fAssets)
        {
            CAppHeader appHeader(g_nAppHeaderVersion, uint256S(g_strSafeAssetId), TRANSFER_ASSET_CMD);
            fCreated = wallet->CreateAssetTransaction(&appHeader, &transferData, vecSend, NULL, NULL, *newTx, *keyChange, nFeeRequired, nChangePosRet, strFailReason, NULL, true, ALL_COINS);
        }
        else
        {
            fCreated = wallet->CreateTransaction(vecSend, *newTx, *keyChange, nFeeRequired, nChangePosRet, strFailReason, coinControl, true, recipients[0].inputType, recipients[0].fUseInstantSend);
        }
        transaction.setTransactionFee(nFeeRequired);
        if (fSubtractFeeFromAmount && fCreated)
            transaction.reassignAmounts(nChangePosRet);

        if(recipients[0].fUseInstantSend) {
            if(newTx->GetValueOut() > sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)*COIN) {
                Q_EMIT message(tr("Send Coins"), tr("InstantSend doesn't support sending high values of transaction inputs yet. Values of transaction inputs are currently limited to %1 SAFE.").arg(sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)),
                             CClientUIInterface::MSG_ERROR);
                return TransactionCreationFailed;
            }
            if(newTx->vin.size() > CTxLockRequest::WARN_MANY_INPUTS) {
                Q_EMIT message(tr("Send Coins"), tr("Used way too many inputs (>%1) for this InstantSend transaction, fees could be huge.").arg(CTxLockRequest::WARN_MANY_INPUTS),
                             CClientUIInterface::MSG_WARNING);
            }
        }

        if(!fCreated)
        {
            if(!fSubtractFeeFromAmount && (total + nFeeRequired) > nBalance)
            {
                return SendCoinsReturn(AmountWithFeeExceedsBalance);
            }
            Q_EMIT message(tr("Send Coins"), QString::fromStdString(strFailReason),
                         CClientUIInterface::MSG_ERROR);
            if(fAssets)
                return None;
            return TransactionCreationFailed;
        }

        // reject absurdly high fee. (This can never happen because the
        // wallet caps the fee at maxTxFee. This merely serves as a
        // belt-and-suspenders check)
        if (nFeeRequired > maxTxFee)
            return AbsurdFee;
    }

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendCoins(WalletModelTransaction &transaction)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);
        CWalletTx *newTx = transaction.getTransaction();
        QList<SendCoinsRecipient> recipients = transaction.getRecipients();

        Q_FOREACH(const SendCoinsRecipient &rcp, recipients)
        {
            if (rcp.paymentRequest.IsInitialized())
            {
                // Make sure any payment requests involved are still valid.
                if (PaymentServer::verifyExpired(rcp.paymentRequest.getDetails())) {
                    return PaymentRequestExpired;
                }

                // Store PaymentRequests in wtx.vOrderForm in wallet.
                std::string key("PaymentRequest");
                std::string value;
                rcp.paymentRequest.SerializeToString(&value);
                newTx->vOrderForm.push_back(make_pair(key, value));
            }
            else if (!rcp.message.isEmpty()) // Message from normal safe:URI (safe:XyZ...?message=example)
            {
                newTx->vOrderForm.push_back(make_pair("Message", rcp.message.toStdString()));
            }
        }

        CReserveKey *keyChange = transaction.getPossibleKeyChange();

        if(!wallet->CommitTransaction(*newTx, *keyChange, g_connman.get(), recipients[0].fUseInstantSend ? NetMsgType::TXLOCKREQUEST : NetMsgType::TX))
        {
            std::vector<int> prevheights;
			/*BOOST_FOREACH(const CTxIn &txin, newTx->vin)
				prevheights.push_back(GetTxHeight(txin.prevout.hash));*/

			BOOST_FOREACH(const CTxIn &txin, newTx->vin)
			{
				std::map<uint256, CWalletTx>::iterator it = wallet->mapWallet.find(txin.prevout.hash);
				if (it != wallet->mapWallet.end())
				{
					prevheights.push_back(it->second.nTxHeight);
				}
			}

            if(ExistForbidTxin((uint32_t)g_nChainHeight + 1, prevheights))
                return TransactionAmountSealed;
            else
                return TransactionCommitFailed;
        }


        CTransaction* t = (CTransaction*)newTx;
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *t;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }

    // Add addresses / update labels that we've sent to to the address book,
    // and emit coinsSent signal for each recipient
    Q_FOREACH(const SendCoinsRecipient &rcp, transaction.getRecipients())
    {
        // Don't touch the address book when we have a payment request
        if (!rcp.paymentRequest.IsInitialized())
        {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = CBitcoinAddress(strAddress).Get();
            std::string strLabel = rcp.label.toStdString();
            {
                LOCK(wallet->cs_wallet);

                std::map<CTxDestination, CAddressBookData>::iterator mi = wallet->mapAddressBook.find(dest);

                // Check if we have a new address or an updated label
                if (mi == wallet->mapAddressBook.end())
                {
                    wallet->SetAddressBook(dest, strLabel, "send");
                }
                else if (mi->second.name != strLabel)
                {
                    wallet->SetAddressBook(dest, strLabel, ""); // "" means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(wallet, rcp, transaction_array);
    }
    //checkBalanceChanged(); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendAssets(WalletModelTransaction &transaction)
{
    if (!pwalletMain)
        return SendCoinsReturn(WalletUnavailable);
    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);
        CWalletTx *newTx = transaction.getTransaction();
        QList<SendCoinsRecipient> recipients = transaction.getRecipients();
        Q_FOREACH(const SendCoinsRecipient &rcp, recipients)
        {
            if (rcp.paymentRequest.IsInitialized())
            {
                // Make sure any payment requests involved are still valid.
                if (PaymentServer::verifyExpired(rcp.paymentRequest.getDetails())) {
                    return PaymentRequestExpired;
                }

                // Store PaymentRequests in wtx.vOrderForm in wallet.
                std::string key("PaymentRequest");
                std::string value;
                rcp.paymentRequest.SerializeToString(&value);
                newTx->vOrderForm.push_back(make_pair(key, value));
            }
            else if (!rcp.message.isEmpty()) // Message from normal safe:URI (safe:XyZ...?message=example)
            {
                newTx->vOrderForm.push_back(make_pair("Message", rcp.message.toStdString()));
            }
        }

        CReserveKey reservekey(wallet);
        CWalletTx& wtx = *transaction.getTransaction();
        if(!wallet->CommitTransaction(wtx, reservekey, g_connman.get()))
            return SendCoinsReturn(CommitTransactionFail);

        CTransaction* t = (CTransaction*)newTx;
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *t;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }


    // Add addresses / update labels that we've sent to to the address book,
    // and emit coinsSent signal for each recipient
    Q_FOREACH(const SendCoinsRecipient &rcp, transaction.getRecipients())
    {
        // Don't touch the address book when we have a payment request
        if (!rcp.paymentRequest.IsInitialized())
        {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = CBitcoinAddress(strAddress).Get();
            std::string strLabel = rcp.label.toStdString();
            {
                LOCK(wallet->cs_wallet);

                std::map<CTxDestination, CAddressBookData>::iterator mi = wallet->mapAddressBook.find(dest);

                // Check if we have a new address or an updated label
                if (mi == wallet->mapAddressBook.end())
                {
                    wallet->SetAddressBook(dest, strLabel, "send");
                }
                else if (mi->second.name != strLabel)
                {
                    wallet->SetAddressBook(dest, strLabel, ""); // "" means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(wallet, rcp, transaction_array);
    }

    //checkBalanceChanged(); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits

    return SendCoinsReturn(OK);
}

OptionsModel *WalletModel::getOptionsModel()
{
    return optionsModel;
}

AddressTableModel *WalletModel::getAddressTableModel()
{
    return addressTableModel;
}

TransactionTableModel *WalletModel::getTransactionTableModel()
{
    return transactionTableModel;
}

TransactionTableModel *WalletModel::getLockedTransactionTableModel()
{
    return lockedTransactionTableModel;
}

TransactionTableModel *WalletModel::getCandyTableModel()
{
    return candyTableModel;
}

TransactionTableModel *WalletModel::getAssetsDistributeTableModel()
{
    return assetsDistributeTableModel;
}

TransactionTableModel *WalletModel::getApplicationRegistTableModel()
{
    return applicationsRegistTableModel;
}

RecentRequestsTableModel *WalletModel::getRecentRequestsTableModel()
{
    return recentRequestsTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    if(!wallet->IsCrypted())
    {
        return Unencrypted;
    }
    else if(wallet->IsLocked(true))
    {
        return Locked;
    }
    else if (wallet->IsLocked())
    {
        return UnlockedForMixingOnly;
    }
    else
    {
        return Unlocked;
    }
}


bool WalletModel::setWalletEncrypted(bool encrypted, const SecureString &passphrase)
{
    if(encrypted)
    {
        // Encrypt
        return wallet->EncryptWallet(passphrase);
    }
    else
    {
        // Decrypt -- TODO; not supported yet
        return false;
    }
}

bool WalletModel::setWalletLocked(bool locked, const SecureString &passPhrase, bool fMixing)
{
    if(locked)
    {
        // Lock
        return wallet->Lock(fMixing);
    }
    else
    {
        // Unlock
        return wallet->Unlock(passPhrase, fMixing);
    }
}

bool WalletModel::changePassphrase(const SecureString &oldPass, const SecureString &newPass)
{
    bool retval;
    {
        LOCK(wallet->cs_wallet);
        wallet->Lock(); // Make sure wallet is locked before attempting pass change
        retval = wallet->ChangeWalletPassphrase(oldPass, newPass);
    }
    return retval;
}

bool WalletModel::backupWallet(const QString &filename)
{
    return BackupWallet(*wallet, filename.toLocal8Bit().data());
}

// Handlers for core signals
static void NotifyKeyStoreStatusChanged(WalletModel *walletmodel, CCryptoKeyStore *wallet)
{
    qDebug() << "NotifyKeyStoreStatusChanged";
    QMetaObject::invokeMethod(walletmodel, "updateStatus", Qt::QueuedConnection);
}

static void NotifyAddressBookChanged(WalletModel *walletmodel, CWallet *wallet,
        const CTxDestination &address, const std::string &label, bool isMine,
        const std::string &purpose, ChangeType status)
{
    QString strAddress = QString::fromStdString(CBitcoinAddress(address).ToString());
    QString strLabel = QString::fromStdString(label);
    QString strPurpose = QString::fromStdString(purpose);

    qDebug() << "NotifyAddressBookChanged: " + strAddress + " " + strLabel + " isMine=" + QString::number(isMine) + " purpose=" + strPurpose + " status=" + QString::number(status);
    QMetaObject::invokeMethod(walletmodel, "updateAddressBook", Qt::QueuedConnection,
                              Q_ARG(QString, strAddress),
                              Q_ARG(QString, strLabel),
                              Q_ARG(bool, isMine),
                              Q_ARG(QString, strPurpose),
                              Q_ARG(int, status));
}

static void NotifyTransactionChanged(WalletModel *walletmodel, CWallet *wallet, const uint256 &hash, ChangeType status)
{
    Q_UNUSED(wallet);
    Q_UNUSED(hash);
    Q_UNUSED(status);
    QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection);
}

static void ShowProgress(WalletModel *walletmodel, const std::string &title, int nProgress)
{
    // emits signal "showProgress"
    QMetaObject::invokeMethod(walletmodel, "showProgress", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(title)),
                              Q_ARG(int, nProgress));
}

static void NotifyWatchonlyChanged(WalletModel *walletmodel, bool fHaveWatchonly)
{
    QMetaObject::invokeMethod(walletmodel, "updateWatchOnlyFlag", Qt::QueuedConnection,
                              Q_ARG(bool, fHaveWatchonly));
}

void WalletModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    wallet->NotifyStatusChanged.connect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyAddressBookChanged.connect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5, _6));
    wallet->NotifyTransactionChanged.connect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->ShowProgress.connect(boost::bind(ShowProgress, this, _1, _2));
    wallet->NotifyWatchonlyChanged.connect(boost::bind(NotifyWatchonlyChanged, this, _1));
}

void WalletModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    wallet->NotifyStatusChanged.disconnect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyAddressBookChanged.disconnect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5, _6));
    wallet->NotifyTransactionChanged.disconnect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->ShowProgress.disconnect(boost::bind(ShowProgress, this, _1, _2));
    wallet->NotifyWatchonlyChanged.disconnect(boost::bind(NotifyWatchonlyChanged, this, _1));
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock(bool fForMixingOnly)
{
    EncryptionStatus encStatusOld = getEncryptionStatus();

    // Wallet was completely locked
    bool was_locked = (encStatusOld == Locked);
    // Wallet was unlocked for mixing
    bool was_mixing = (encStatusOld == UnlockedForMixingOnly);
    // Wallet was unlocked for mixing and now user requested to fully unlock it
    bool fMixingToFullRequested = !fForMixingOnly && was_mixing;

    if(was_locked || fMixingToFullRequested) {
        // Request UI to unlock wallet
        Q_EMIT requireUnlock(fForMixingOnly);
    }

    EncryptionStatus encStatusNew = getEncryptionStatus();

    // Wallet was locked, user requested to unlock it for mixing and failed to do so
    bool fMixingUnlockFailed = fForMixingOnly && !(encStatusNew == UnlockedForMixingOnly);
    // Wallet was unlocked for mixing, user requested to fully unlock it and failed
    bool fMixingToFullFailed = fMixingToFullRequested && !(encStatusNew == Unlocked);
    // If wallet is still locked, unlock failed or was cancelled, mark context as invalid
    bool fInvalid = (encStatusNew == Locked) || fMixingUnlockFailed || fMixingToFullFailed;
    // Wallet was not locked in any way or user tried to unlock it for mixing only and succeeded, keep it unlocked
    bool fKeepUnlocked = !was_locked || (fForMixingOnly && !fMixingUnlockFailed);

    return UnlockContext(this, !fInvalid, !fKeepUnlocked, was_mixing);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *wallet, bool valid, bool was_locked, bool was_mixing):
        wallet(wallet),
        valid(valid),
        was_locked(was_locked),
        was_mixing(was_mixing)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if(valid && (was_locked || was_mixing))
    {
        wallet->setWalletLocked(true, "", was_mixing);
    }
}

void WalletModel::UnlockContext::CopyFrom(const UnlockContext& rhs)
{
    // Transfer context; old object no longer relocks wallet
    *this = rhs;
    rhs.was_locked = false;
    rhs.was_mixing = false;
}

bool WalletModel::getPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const
{
    return wallet->GetPubKey(address, vchPubKeyOut);
}

bool WalletModel::havePrivKey(const CKeyID &address) const
{
    return wallet->HaveKey(address);
}

// returns a list of COutputs from COutPoints
void WalletModel::getOutputs(const std::vector<COutPoint>& vOutpoints, std::vector<COutput>& vOutputs)
{
    LOCK2(cs_main, wallet->cs_wallet);
    BOOST_FOREACH(const COutPoint& outpoint, vOutpoints)
    {
        if (!wallet->mapWallet.count(outpoint.hash)) continue;
        int nDepth = wallet->mapWallet[outpoint.hash].GetDepthInMainChain();
        if (nDepth < 0) continue;
        COutput out(&wallet->mapWallet[outpoint.hash], outpoint.n, nDepth, true, true);
        vOutputs.push_back(out);
    }
}

bool WalletModel::isSpent(const COutPoint& outpoint) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsSpent(outpoint.hash, outpoint.n);
}

// AvailableCoins + FrozenCoins grouped by wallet address (put change in one group with wallet address)
void WalletModel::listCoins(std::map<QString, std::vector<COutput> >& mapCoins) const
{
    std::vector<COutput> vCoins;
    wallet->AvailableCoins(vCoins, true, NULL, false, ALL_COINS, false, true);

    LOCK2(cs_main, wallet->cs_wallet); // ListFrozenCoins, mapWallet
    std::vector<COutPoint> vFrozenCoins;
    wallet->ListFrozenCoins(vFrozenCoins);

    // add frozen coins
    BOOST_FOREACH(const COutPoint& outpoint, vFrozenCoins)
    {
        if (!wallet->mapWallet.count(outpoint.hash)) continue;
        int nDepth = wallet->mapWallet[outpoint.hash].GetDepthInMainChain();
        if (nDepth < 0) continue;
        COutput out(&wallet->mapWallet[outpoint.hash], outpoint.n, nDepth, true, true);
        if (outpoint.n < out.tx->vout.size() && wallet->IsMine(out.tx->vout[outpoint.n]) == ISMINE_SPENDABLE)
            vCoins.push_back(out);
    }

    BOOST_FOREACH(const COutput& out, vCoins)
    {
        COutput cout = out;

        while (wallet->IsChange(cout.tx->vout[cout.i]) && cout.tx->vin.size() > 0 && wallet->IsMine(cout.tx->vin[0]))
        {
            if (!wallet->mapWallet.count(cout.tx->vin[0].prevout.hash)) break;
            cout = COutput(&wallet->mapWallet[cout.tx->vin[0].prevout.hash], cout.tx->vin[0].prevout.n, 0, true, true);
        }

        CTxDestination address;
        if(!out.fSpendable || !ExtractDestination(cout.tx->vout[cout.i].scriptPubKey, address))
            continue;
        mapCoins[QString::fromStdString(CBitcoinAddress(address).ToString())].push_back(out);
    }
}

bool WalletModel::isFrozenCoin(uint256 hash, unsigned int n) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsFrozenCoin(hash, n);
}

void WalletModel::freezeCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->FreezeCoin(output);
}

void WalletModel::unfreezeCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->UnfreezeCoin(output);
}

void WalletModel::listFrozenCoins(std::vector<COutPoint>& vOutpts)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->ListFrozenCoins(vOutpts);
}

void WalletModel::loadReceiveRequests(std::vector<std::string>& vReceiveRequests)
{
    LOCK(wallet->cs_wallet);
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, CAddressBookData)& item, wallet->mapAddressBook)
        BOOST_FOREACH(const PAIRTYPE(std::string, std::string)& item2, item.second.destdata)
            if (item2.first.size() > 2 && item2.first.substr(0,2) == "rr") // receive request
                vReceiveRequests.push_back(item2.second);
}

bool WalletModel::saveReceiveRequest(const std::string &sAddress, const int64_t nId, const std::string &sRequest)
{
    CTxDestination dest = CBitcoinAddress(sAddress).Get();

    std::stringstream ss;
    ss << nId;
    std::string key = "rr" + ss.str(); // "rr" prefix = "receive request" in destdata

    LOCK(wallet->cs_wallet);
    if (sRequest.empty())
        return wallet->EraseDestData(dest, key);
    else
        return wallet->AddDestData(dest, key, sRequest);
}

bool WalletModel::transactionCanBeAbandoned(uint256 hash) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    const CWalletTx *wtx = wallet->GetWalletTx(hash);
    if (!wtx || wtx->isAbandoned() || wtx->GetDepthInMainChain() > 0 || wtx->InMempool())
        return false;
    return true;
}

bool WalletModel::abandonTransaction(uint256 hash) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->AbandonTransaction(hash);
}

bool WalletModel::hdEnabled() const
{
    return wallet->IsHDEnabled();
}

void WalletModel::setWalletView(WalletView *walletView)
{
	pWalletView = walletView;
}

void EncryptWorker::doEncrypt()
{
    // Encrypt
//    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    bool ret = model->setWalletEncrypted(true, passphrase);
//    QApplication::restoreOverrideCursor();
    Q_EMIT resultReady(ret);
}

void WalletModel::loadHistroyData()
{
	QMap<QString, AssetsDisplayInfo> mapAssetList;
	QList<TransactionRecord> listTransaction;
	QMap<uint256, CAssetData> mapIssueAsset;

	{
		LOCK2(cs_main, wallet->cs_wallet);
		for (std::map<uint256, CWalletTx>::iterator it = wallet->mapWallet.begin(); it != wallet->mapWallet.end(); ++it)
		{
			boost::this_thread::interruption_point();
			if (TransactionRecord::showTransaction(it->second))
			{
				TransactionRecord::decomposeTransaction(wallet, it->second, listTransaction, mapAssetList, mapIssueAsset);
			}
		}
	}

	int nTxStart = 0, nTxCount = 0;
	int nAssetStart = 0, nAssetCount = 0;
	int nAppStart = 0, nAppCount = 0;
	int nCandyStart = 0, nCandyCount = 0;
	int nLockStart = 0, nLockCount = 0;

	for (int i = 0; i < listTransaction.size(); i++)
	{
		boost::this_thread::interruption_point();
		for (int j = 0; j < listTransaction[i].vtShowType.size(); j++)
		{
			if (listTransaction[i].vtShowType[j] == SHOW_TX)
			{
				nTxCount++;
			}
			else if (listTransaction[i].vtShowType[j] == SHOW_ASSETS_DISTRIBUTE)
			{
				nAssetCount++;
			}
			else if (listTransaction[i].vtShowType[j] == SHOW_APPLICATION_REGIST)
			{
				nAppCount++;
			}
			else if (listTransaction[i].vtShowType[j] == SHOW_CANDY_TX)
			{
				nCandyCount++;
			}
			else if (listTransaction[i].vtShowType[j] == SHOW_LOCKED_TX)
			{
				nLockCount++;
			}
		}
	}

	if (nTxCount > MAX_TX_DISPLAY)
	{
		nTxStart = nTxCount - MAX_TX_DISPLAY;
	}

	if (nAssetCount > MAX_TX_DISPLAY)
	{
		nAssetStart = nAssetCount - MAX_TX_DISPLAY;
	}

	if (nAppCount > MAX_TX_DISPLAY)
	{
		nAppStart = nAppCount - MAX_TX_DISPLAY;
	}

	if (nCandyCount > MAX_TX_DISPLAY)
	{
		nCandyStart = nCandyCount - MAX_TX_DISPLAY;
	}

	if (nLockCount > MAX_TX_DISPLAY)
	{
		nLockStart = nLockCount - MAX_TX_DISPLAY;
	}


	for (int i = 0; i < listTransaction.size(); i++)
	{
		boost::this_thread::interruption_point();
		for (int j = 0; j < listTransaction[i].vtShowType.size(); j++)
		{
			if (listTransaction[i].vtShowType[j] == SHOW_TX)
			{
				if (i >= nTxStart)
				{
					transactionTableModel->insertTransaction(listTransaction[i]);
				}
			}
			else if (listTransaction[i].vtShowType[j] == SHOW_ASSETS_DISTRIBUTE)
			{
				if (i >= nAssetStart)
				{
					assetsDistributeTableModel->insertTransaction(listTransaction[i]);
				}
			}
			else if (listTransaction[i].vtShowType[j] == SHOW_APPLICATION_REGIST)
			{
				if (i >= nAppStart)
				{
					applicationsRegistTableModel->insertTransaction(listTransaction[i]);
				}
			}
			else if (listTransaction[i].vtShowType[j] == SHOW_CANDY_TX)
			{
				if (i >= nCandyStart)
				{
					candyTableModel->insertTransaction(listTransaction[i]);
				}
			}
			else if (listTransaction[i].vtShowType[j] == SHOW_LOCKED_TX)
			{
				if (i >= nLockStart)
				{
					lockedTransactionTableModel->insertTransaction(listTransaction[i]);
				}
			}
		}
	}

	transactionTableModel->sortData();
	assetsDistributeTableModel->sortData();
	applicationsRegistTableModel->sortData();
	candyTableModel->sortData();
	lockedTransactionTableModel->sortData();

	if (mapAssetList.size() > 0)
	{
		Q_EMIT pUpdateTransaction->updateAssetDisplayInfo(mapAssetList);
		pUpdateTransaction->RefreshOverviewPageData(mapAssetList.keys());
	}

	if (mapIssueAsset.size() > 0)
	{
		pUpdateTransaction->RefreshAssetData(mapIssueAsset);
		pUpdateTransaction->RefreshCandyPageData(mapIssueAsset);
	}

	Q_EMIT loadWalletFinish();
}


void ThreadUpdateBalanceChanged(WalletModel* walletModel)
{
	static bool fOneThread = false;
	if (fOneThread) {
		return;
	}

	fOneThread = true;
	if (NULL == walletModel) {
		return;
	}

	RenameThread("updateBalanceChangedThread");
	LogPrintf("guidebug_message:ThreadUpdateBalanceChanged is start\n");
	while (true) {
		boost::this_thread::interruption_point();
		walletModel->pollBalanceChanged(true);
		MilliSleep(MODEL_UPDATE_DELAY);
	}
}

void WalletModel::ShowHistoryPage()
{
	loadHistroyData();
}

CUpdateTransaction *WalletModel::getUpdateTransaction()
{
	return pUpdateTransaction;
}

void WalletModel::startUpdate()
{
	g_threadGroup->create_thread(boost::bind(&ThreadUpdateBalanceChanged, this));
}

void WalletModel::updateAllTransaction_slot(const QMap<uint256, QList<TransactionRecord> > &mapDecTransaction, const QMap<uint256, NewTxData> &mapTransactionStatus)
{
	QMap<uint256, QList<TransactionRecord> >::const_iterator itTx = mapDecTransaction.begin();
	while (itTx != mapDecTransaction.end())
	{
		this->mapDecTransaction.insert(itTx.key(), itTx.value());
		itTx++;
	}

	QMap<uint256, NewTxData>::const_iterator itStatus = mapTransactionStatus.begin();
	while (itStatus != mapTransactionStatus.end())
	{
		this->mapTransactionStatus.insert(itStatus.key(), itStatus.value());
		itStatus++;
	}

	if (!pTimer->isActive())
	{
		pTimer->start(250);
	}
}

void WalletModel::refreshTransaction_slot()
{
	bool bTxRefresh = false;
	bool bAssetRefresh = false;
	bool bAppRefresh = false;
	bool bLockRefresh = false;
	bool bCandyRefresh = false;

	QString strDebug = "WalletModel::refreshTransaction_slot, ---start---, txCount: " + QString::number(mapDecTransaction.size());
	qDebug() << strDebug;

	QMap<uint256, QList<TransactionRecord> >::iterator itTx = mapDecTransaction.begin();
	for (; itTx != mapDecTransaction.end(); itTx++)
	{
		QList<TransactionRecord> listTx;
		QList<TransactionRecord> listAssetTx;
		QList<TransactionRecord> listAppTx;
		QList<TransactionRecord> listCandyTx;
		QList<TransactionRecord> listLockTx;
		const QList<TransactionRecord> &listToInsert = itTx.value();

		for (int i = 0; i < listToInsert.size(); i++)
		{
			for (int j = 0; j < listToInsert[i].vtShowType.size(); j++)
			{
				if (listToInsert[i].vtShowType[j] == SHOW_TX)
				{
					listTx.push_back(listToInsert[i]);
				}
				else if (listToInsert[i].vtShowType[j] == SHOW_ASSETS_DISTRIBUTE)
				{
					listAssetTx.push_back(listToInsert[i]);
				}
				else if (listToInsert[i].vtShowType[j] == SHOW_APPLICATION_REGIST)
				{
					listAppTx.push_back(listToInsert[i]);
				}
				else if (listToInsert[i].vtShowType[j] == SHOW_CANDY_TX)
				{
					listCandyTx.push_back(listToInsert[i]);
				}
				else if (listToInsert[i].vtShowType[j] == SHOW_LOCKED_TX)
				{
					listLockTx.push_back(listToInsert[i]);
				}
			}
		}


		QMap<uint256, NewTxData>::iterator itNew = mapTransactionStatus.find(itTx.key());
		if (itNew == mapTransactionStatus.end())
		{
			continue;
		}

		uint256 hash = itNew.key();
		const NewTxData &stTxData = itNew.value();

		if (listTx.size() > 0)
		{
			transactionTableModel->updateTransaction(hash, listTx, stTxData.nStatus, stTxData.bShowTx, bTxRefresh);
		}

		if (listAssetTx.size() > 0)
		{
			assetsDistributeTableModel->updateTransaction(hash, listAssetTx, stTxData.nStatus, stTxData.bShowTx, bAssetRefresh);
		}

		if (listAppTx.size() > 0)
		{
			applicationsRegistTableModel->updateTransaction(hash, listAppTx, stTxData.nStatus, stTxData.bShowTx, bAppRefresh);
		}

		if (listCandyTx.size() > 0)
		{
			candyTableModel->updateTransaction(hash, listCandyTx, stTxData.nStatus, stTxData.bShowTx, bCandyRefresh);
		}

		if (listLockTx.size() > 0)
		{
			lockedTransactionTableModel->updateTransaction(hash, listLockTx, stTxData.nStatus, stTxData.bShowTx, bLockRefresh);
		}
	}

	mapDecTransaction.clear();
	mapTransactionStatus.clear();

	if (bTxRefresh)
	{
		pWalletView->refreshTransactionView();
	}

	if (bAssetRefresh)
	{
		pWalletView->refreshAssetTransactionView();
	}

	if (bAppRefresh)
	{
		pWalletView->refreshAppTransactionView();
	}

	if (bCandyRefresh)
	{
		pWalletView->refreshCandyTransactionView();
	}

	if (bLockRefresh)
	{
		pWalletView->refreshLockTransactionView();
	}

	pTimer->stop();

	strDebug = "WalletModel::refreshTransaction_slot, ---end---, ";
	strDebug += "bTxRefresh: " + QString::number(bTxRefresh) + ", ";
	strDebug += "bAssetRefresh: " + QString::number(bAssetRefresh) + ", ";
	strDebug += "bAppRefresh: " + QString::number(bAppRefresh) + ", ";
	strDebug += "bCandyRefresh: " + QString::number(bCandyRefresh) + ", ";
	strDebug += "bLockRefresh: " + QString::number(bLockRefresh);
	qDebug() << strDebug;
}
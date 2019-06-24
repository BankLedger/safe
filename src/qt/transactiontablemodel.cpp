// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "transactiontablemodel.h"

#include "addresstablemodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "platformstyle.h"
#include "transactiondesc.h"
#include "walletmodel.h"

#include "chainparams.h"
#include "core_io.h"
#include "main.h"
#include "sync.h"
#include "uint256.h"
#include "util.h"
#include "validation.h"
#include "wallet/wallet.h"

#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QIcon>
#include <QList>
#include <QThread>

#include <boost/foreach.hpp>
#include <boost/thread.hpp>


// Amount column is right-aligned it contains numbers
static int column_alignments[] = {
    Qt::AlignLeft | Qt::AlignVCenter, /* status */
    Qt::AlignLeft | Qt::AlignVCenter, /* watchonly */
    Qt::AlignLeft | Qt::AlignVCenter, /* date */
    Qt::AlignLeft | Qt::AlignVCenter, /* type */
    Qt::AlignLeft | Qt::AlignVCenter, /* address */
    Qt::AlignLeft | Qt::AlignVCenter, /* assets name */
    Qt::AlignRight | Qt::AlignVCenter /* amount */
};

// Comparison operator for sort/binary search of model tx list
struct TxLessThan {
    bool operator()(const TransactionRecord& a, const TransactionRecord& b) const
    {
        return a.hash < b.hash;
    }
    bool operator()(const TransactionRecord& a, const uint256& b) const
    {
        return a.hash < b;
    }
    bool operator()(const uint256& a, const TransactionRecord& b) const
    {
        return a < b.hash;
    }
};


void TransactionTablePriv::updateWallet(uint256 hash, QList<TransactionRecord> listNew, int status, bool showTransaction)
{
	qDebug() << "TransactionTablePriv::updateWallet: " + QString::fromStdString(hash.ToString()) + " " + QString::number(status);

	// Find bounds of this transaction in model
	QList<TransactionRecord>::iterator lower = qLowerBound(
		cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
	QList<TransactionRecord>::iterator upper = qUpperBound(
		cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
	int lowerIndex = (lower - cachedWallet.begin());
	int upperIndex = (upper - cachedWallet.begin());
	bool inModel = (lower != upper);

	bool fReupdate = false;

	if (status == CT_UPDATED) {
		if (showTransaction && !inModel)
			status = CT_NEW; /* Not in model, but want to show, treat as new */
		if (!showTransaction && inModel)
			status = CT_DELETED; /* In model, but want to hide, treat as deleted */
		if (showType == SHOW_LOCKED_TX && inModel && showTransaction && status == CT_UPDATED) {
			std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(hash);
			const CWalletTx& wtx = mi->second;
			for (unsigned int nOut = 0; nOut < wtx.vout.size(); nOut++) {
				const CTxOut& txout = wtx.vout[nOut];
				if (txout.nUnlockedHeight <= 0)
					continue;
				status = CT_NEW;
				fReupdate = true;
				break;
			}
		}
	}

	qDebug() << "    inModel=" + QString::number(inModel) +
		" Index=" + QString::number(lowerIndex) + "-" + QString::number(upperIndex) +
		" showTransaction=" + QString::number(showTransaction) + " derivedStatus=" + QString::number(status);

	switch (status) {
	case CT_NEW:
		if (inModel) {
			qWarning() << "TransactionTablePriv::updateWallet: Warning: Got CT_NEW, but transaction is already in model";
			if (!fReupdate)
				break;
		}
		if (showTransaction) {

			if (fReupdate) {
				for (int i = 0; i < cachedWallet.size(); i++) {
					TransactionRecord& rec = cachedWallet[i];
					for (int j = 0; j < listNew.size(); j++) {
						TransactionRecord& toInsertRec = listNew[j];
						if (rec.bLocked && rec.hash == hash) {
							rec.nTxHeight = toInsertRec.nTxHeight;
							rec.updateLockedMonth();
						}
					}
				}
			}
			else {
				//insert new data
				parent->beginInsertRows(QModelIndex(), lowerIndex, lowerIndex + listNew.size() - 1);
				int insert_idx = lowerIndex;
				Q_FOREACH(const TransactionRecord& rec, listNew) {
					cachedWallet.insert(insert_idx, rec);
					insert_idx += 1;
				}
				parent->endInsertRows();
			}

		}
		break;
	case CT_DELETED:
		if (!inModel) {
			qWarning() << "TransactionTablePriv::updateWallet: Warning: Got CT_DELETED, but transaction is not in model";
			break;
		}
		// Removed -- remove entire transaction from table
		parent->beginRemoveRows(QModelIndex(), lowerIndex, upperIndex - 1);
		cachedWallet.erase(lower, upper);
		parent->endRemoveRows();
		break;
	case CT_UPDATED:
		// Miscellaneous updates -- nothing to do, status update will take care of this, and is only computed for
		// visible transactions.
		break;
	}
}

int TransactionTablePriv::size()
{
    return cachedWallet.size();
}

TransactionRecord* TransactionTablePriv::index(int idx)
{
    if (idx >= 0 && idx < cachedWallet.size())
	{
        TransactionRecord* rec = &cachedWallet[idx];

        // Get required locks upfront. This avoids the GUI from getting
        // stuck if the core is holding the locks for a longer time - for
        // example, during a wallet rescan.
        //
        // If a status update is needed (blocks came in since last check),
        //  update the status of this transaction from the wallet. Otherwise,
        // simply re-use the cached status.
        TRY_LOCK(cs_main, lockMain);
        if (lockMain)
		{
            TRY_LOCK(wallet->cs_wallet, lockWallet);
            if (lockWallet && rec->statusUpdateNeeded())
			{
                std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(rec->hash);
                if (mi != wallet->mapWallet.end()) 
				{
                    rec->updateStatus(mi->second);
				}
            }
        }

        return rec;
    }

    return 0;
}

QString TransactionTablePriv::describe(TransactionRecord* rec, int unit, bool fAssets)
{
    {
        LOCK2(cs_main, wallet->cs_wallet);
        std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(rec->hash);
        if (mi != wallet->mapWallet.end()) {
            return TransactionDesc::toHTML(wallet, mi->second, rec, unit, showType, fAssets);
        }
    }
    return QString();
}

QString TransactionTablePriv::getTxHex(TransactionRecord* rec)
{
    LOCK2(cs_main, wallet->cs_wallet);
    std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(rec->hash);
    if (mi != wallet->mapWallet.end()) {
        std::string strHex = EncodeHexTx(static_cast<CTransaction>(mi->second));
        return QString::fromStdString(strHex);
    }
    return QString();
}

void TransactionTablePriv::insertTransaction(const TransactionRecord &tr)
{
	cachedWallet.push_back(tr);
}

void TransactionTablePriv::clearData()
{
	cachedWallet.clear();
	QList<TransactionRecord>().swap(cachedWallet);
}

TransactionTableModel::TransactionTableModel(const PlatformStyle* platformStyle, CWallet* wallet, int showType, WalletModel* parent) :
	QAbstractTableModel(parent),
	wallet(wallet),
	walletModel(parent),
	priv(new TransactionTablePriv(wallet, showType, this)),
	platformStyle(platformStyle),
	showType(showType)
{
    columns << QString() << QString() << tr("Date") << tr("Type") << tr("Address / Label") << tr("Assets name") << BitcoinUnits::getAmountColumnTitle(walletModel->getOptionsModel()->getDisplayUnit());

    columnStatus = TransactionTableModel::TransactionColumnStatus;
    columnToAddress = TransactionTableModel::TransactionColumnToAddress;
    columnAmount = TransactionTableModel::TransactionColumnAmount;
    nUpdateCount = 0;

    connect(walletModel->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
}

TransactionTableModel::~TransactionTableModel()
{
    delete priv;
}

/** Updates the column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
void TransactionTableModel::updateAmountColumnTitle()
{
    if (columns.size() <= columnAmount)
        return;
    if (columnAmount < 0)
        return;
    columns[columnAmount] = BitcoinUnits::getAmountColumnTitle(walletModel->getOptionsModel()->getDisplayUnit());
    Q_EMIT headerDataChanged(Qt::Horizontal, columnAmount, columnAmount);
}

void TransactionTableModel::updateTransaction(uint256 hash, QList<TransactionRecord> listNew, int status, bool showTransaction)
{
	priv->updateWallet(hash, listNew, status, showTransaction);
}

void TransactionTableModel::updateConfirmations()
{
    // Blocks came in since last poll.
    // Invalidate status (number of confirmations) and (possibly) description
    //  for all rows. Qt is smart enough to only actually request the data for the
    //  visible rows.
    nUpdateCount++;
    if (nUpdateCount > 10000) {
        nUpdateCount = 10000;
    }

    if (nUpdateCount < 30) {
        return;
    }

    int size = priv->size() - 1;
    if (size > 0) {
        Q_EMIT dataChanged(index(0, columnStatus), index(size, columnStatus));
        Q_EMIT dataChanged(index(0, columnToAddress), index(size, columnToAddress));
    }
}

int TransactionTableModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int TransactionTableModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return columns.length();
}

QString TransactionTableModel::formatTxStatus(const TransactionRecord* wtx) const
{
    QString status;

    switch (wtx->status.status) {
    case TransactionStatus::OpenUntilBlock:
        status = tr("Open for %n more block(s)", "", wtx->status.open_for);
        break;
    case TransactionStatus::OpenUntilDate:
        status = tr("Open until %1").arg(GUIUtil::dateTimeStr(wtx->status.open_for));
        break;
    case TransactionStatus::Offline:
        status = tr("Offline");
        break;
    case TransactionStatus::Unconfirmed:
        status = tr("Unconfirmed");
        break;
    case TransactionStatus::Abandoned:
        status = tr("Abandoned");
        break;
    case TransactionStatus::Confirming:
        status = tr("Confirming (%1 of %2 recommended confirmations)").arg(wtx->status.depth).arg(TransactionRecord::RecommendedNumConfirmations);
        break;
    case TransactionStatus::Confirmed:
        status = tr("Confirmed (%1 confirmations)").arg(wtx->status.depth);
        break;
    case TransactionStatus::Conflicted:
        status = tr("Conflicted");
        break;
    case TransactionStatus::Immature:
        status = tr("Immature (%1 confirmations, will be available after %2)").arg(wtx->status.depth).arg(wtx->status.depth + wtx->status.matures_in);
        break;
    case TransactionStatus::MaturesWarning:
        status = tr("This block was not received by any other nodes and will probably not be accepted!");
        break;
    case TransactionStatus::NotAccepted:
        status = tr("Generated but not accepted");
        break;
    }

    return status;
}

QString TransactionTableModel::formatTxDate(const TransactionRecord* wtx) const
{
    if (wtx->time) {
        return GUIUtil::dateTimeStr(wtx->time);
    }
    return QString();
}

static int TimeDiff(const int64_t nStartTime, const int64_t nEndTime)
{
    boost::posix_time::ptime t1 = boost::posix_time::from_time_t(nStartTime);
    int year1 = t1.date().year();
    int month1 = t1.date().month();
    boost::posix_time::ptime t2 = boost::posix_time::from_time_t(nEndTime);
    int year2 = t2.date().year();
    int month2 = t2.date().month();
    return (year2 - year1) * 12 + (month2 - month1);
}

QString TransactionTableModel::formatAssetsDistributeType(const TransactionRecord* wtx) const
{
    QString str = "unknown type";
    if (wtx->type == TransactionRecord::FirstDistribute)
        str = tr("First Distribute");
    else if (wtx->type == TransactionRecord::AddDistribute)
        str = tr("Add Distribute");
    return str;
}

QString TransactionTableModel::formatAssetsAmount(const TransactionRecord* wtx, bool showUnconfirmed, BitcoinUnits::SeparatorStyle separators) const
{
    QString str = "";
    if (wtx->type == TransactionRecord::FirstDistribute)
        str = BitcoinUnits::formatWithUnit(wtx->assetsData.nDecimals, wtx->assetsData.nFirstIssueAmount, false, separators, true, QString::fromStdString(wtx->assetsData.strAssetUnit));
    else if (wtx->type == TransactionRecord::AddDistribute)
        str = BitcoinUnits::formatWithUnit(wtx->assetsData.nDecimals, wtx->commonData.nAmount, false, separators, true, QString::fromStdString(wtx->assetsData.strAssetUnit));
    if (showUnconfirmed) {
        if (!wtx->status.countsForBalance) {
            str = QString("[") + str + QString("]");
        }
    }
    return str;
}

QString TransactionTableModel::formatCandyAmount(const TransactionRecord* wtx, bool showUnconfirmed, BitcoinUnits::SeparatorStyle separators) const
{
    QString str = BitcoinUnits::formatWithUnit(wtx->assetsData.nDecimals, wtx->getCandyData.nAmount, false, separators, true, QString::fromStdString(wtx->assetsData.strAssetUnit));
    if (showUnconfirmed) {
        if (!wtx->status.countsForBalance) {
            str = QString("[") + str + QString("]");
        }
    }
    return str;
}

QString TransactionTableModel::formatAssetsName(const TransactionRecord* wtx) const
{
    if (wtx->bAssets || wtx->bGetCandy || wtx->bPutCandy)
        return QString::fromStdString(wtx->assetsData.strAssetName);
    if (wtx->bSAFETransaction)
        return tr("SAFE");
    return tr("");
}

QString TransactionTableModel::formatAssetsAddress(const TransactionRecord* wtx) const
{
    return QString::fromStdString(wtx->address);
}

QString TransactionTableModel::formatLockedTxAmount(const TransactionRecord* wtx, bool showUnconfirmed, BitcoinUnits::SeparatorStyle separators) const
{
    QString str = "";
    if (wtx->bSAFETransaction)
        str = BitcoinUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), wtx->nLockedAmount, false, separators);
    else
        str = BitcoinUnits::format(wtx->assetsData.nDecimals, wtx->nLockedAmount, false, separators, true);
    if (showUnconfirmed) {
        if (!wtx->status.countsForBalance) {
            str = QString("[") + str + QString("]");
        }
    }

    QString unit = "";
    if (wtx->bSAFETransaction)
        unit = BitcoinUnits::name(walletModel->getOptionsModel()->getDisplayUnit());
    else
        unit = QString::fromStdString(wtx->assetsData.strAssetUnit);
    return QString(str.append(" ").append(unit));
}

QString TransactionTableModel::formatLockedMonth(const TransactionRecord* rec) const
{
    if (rec->nUnlockedHeight > 0)
        return rec->strLockedMonth;
    return QString();
}

QString TransactionTableModel::formatUnlockedHeight(const TransactionRecord* wtx) const
{
    int64_t nRealUnlockHeight = wtx->getRealUnlockHeight();
    if (nRealUnlockHeight != 0)
        return QString::number(nRealUnlockHeight);
    return QString();
}

QString TransactionTableModel::formatLockedStatus(const TransactionRecord* wtx) const
{
    if (wtx->status.status == TransactionStatus::Conflicted)
        return tr("Invalid: Conflicted");
    if (wtx->getRealUnlockHeight() <= g_nChainHeight)
        return tr("Unlocked");
    else
        return tr("Locking");
}

/* Look up address in address book, if found return label (address)
   otherwise just return (address)
 */
QString TransactionTableModel::lookupAddress(const std::string& address, bool tooltip) const
{
    QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(address));
    QString description;
    if (!label.isEmpty()) {
        description += label;
    }
    if (label.isEmpty() || tooltip) {
        description += QString(" (") + QString::fromStdString(address) + QString(")");
    }
    return description;
}

QString TransactionTableModel::formatTxType(const TransactionRecord* wtx) const
{
    QString ret = "";
    switch (wtx->type) {
    case TransactionRecord::RecvWithAddress:
        ret = tr("Received with");
        break;
    case TransactionRecord::RecvFromOther:
    case TransactionRecord::GETCandy:
        ret = tr("Received from");
        break;
    case TransactionRecord::RecvWithPrivateSend:
        ret = tr("Received via PrivateSend");
        break;
    case TransactionRecord::SendToAddress:
    case TransactionRecord::SendToOther:
    case TransactionRecord::FirstDistribute:
    case TransactionRecord::AddDistribute:
    case TransactionRecord::PUTCandy:
        ret = tr("Sent to");
        break;
    case TransactionRecord::SendToSelf:
        ret = tr("Payment to yourself");
        break;
    case TransactionRecord::Generated:
        ret = tr("Mined");
        break;
    case TransactionRecord::PrivateSendDenominate:
        ret = tr("PrivateSend Denominate");
        break;
    case TransactionRecord::PrivateSendCollateralPayment:
        ret = tr("PrivateSend Collateral Payment");
        break;
    case TransactionRecord::PrivateSendMakeCollaterals:
        ret = tr("PrivateSend Make Collateral Inputs");
        break;
    case TransactionRecord::PrivateSendCreateDenominations:
        ret = tr("PrivateSend Create Denominations");
        break;
    case TransactionRecord::PrivateSend:
        ret = tr("PrivateSend");
        break;
    default:
        break;
    }
    if (wtx->bForbidDash)
        ret.append(tr(" [sealed]"));
    return ret;
}

QVariant TransactionTableModel::txAddressDecoration(const TransactionRecord* wtx) const
{
    QString theme = GUIUtil::getThemeName();
    switch (wtx->type) {
    case TransactionRecord::Generated:
        return QIcon(":/icons/" + theme + "/tx_mined");
    case TransactionRecord::RecvWithPrivateSend:
    case TransactionRecord::RecvWithAddress:
    case TransactionRecord::RecvFromOther:
        return QIcon(":/icons/" + theme + "/tx_input");
    case TransactionRecord::SendToAddress:
    case TransactionRecord::SendToOther:
        return QIcon(":/icons/" + theme + "/tx_output");
    default:
        return QIcon(":/icons/" + theme + "/tx_inout");
    }
}

QString TransactionTableModel::formatTxToAddress(const TransactionRecord* wtx, bool tooltip) const
{
    QString watchAddress;
    if (tooltip) {
        // Mark transactions involving watch-only addresses by adding " (watch-only)"
        watchAddress = wtx->involvesWatchAddress ? QString(" (") + tr("watch-only") + QString(")") : "";
    }

    switch (wtx->type) {
    case TransactionRecord::RecvFromOther:
        return QString::fromStdString(wtx->address) + watchAddress;
    case TransactionRecord::FirstDistribute:
    case TransactionRecord::AddDistribute:
    case TransactionRecord::GETCandy:
    case TransactionRecord::PUTCandy:
    case TransactionRecord::RecvWithAddress:
    case TransactionRecord::RecvWithPrivateSend:
    case TransactionRecord::SendToAddress:
    case TransactionRecord::Generated:
    case TransactionRecord::PrivateSend:
        return lookupAddress(wtx->address, tooltip) + watchAddress;
    case TransactionRecord::SendToOther:
        return QString::fromStdString(wtx->address) + watchAddress;
    case TransactionRecord::SendToSelf:
    default:
        return tr("(n/a)") + watchAddress;
    }
}

QVariant TransactionTableModel::addressColor(const TransactionRecord* wtx) const
{
    // Show addresses without label in a less visible color
    switch (wtx->type) {
    case TransactionRecord::RecvWithAddress:
    case TransactionRecord::SendToAddress:
    case TransactionRecord::Generated:
    case TransactionRecord::PrivateSend:
    case TransactionRecord::RecvWithPrivateSend: {
        QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(wtx->address));
        if (label.isEmpty())
            return COLOR_BAREADDRESS;
    } break;
    case TransactionRecord::SendToSelf:
    case TransactionRecord::PrivateSendCreateDenominations:
    case TransactionRecord::PrivateSendDenominate:
    case TransactionRecord::PrivateSendMakeCollaterals:
    case TransactionRecord::PrivateSendCollateralPayment:
        return COLOR_BAREADDRESS;
    default:
        break;
    }
    return QVariant();
}

QString TransactionTableModel::formatTxAmount(const TransactionRecord* wtx, bool showUnconfirmed, BitcoinUnits::SeparatorStyle separators) const
{
    QString str = "";
    if (wtx->bSAFETransaction)
        str = BitcoinUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), wtx->credit + wtx->debit, false, separators);
    else
        str = BitcoinUnits::format(wtx->assetsData.nDecimals, wtx->assetCredit + wtx->assetDebit, false, separators, true);
    if (showUnconfirmed) {
        if (!wtx->status.countsForBalance) {
            str = QString("[") + str + QString("]");
        }
    }

    QString unit = BitcoinUnits::name(walletModel->getOptionsModel()->getDisplayUnit());
    if (wtx->bSAFETransaction)
        unit = BitcoinUnits::name(walletModel->getOptionsModel()->getDisplayUnit());
    else
        unit = QString::fromStdString(wtx->assetsData.strAssetUnit);
    return QString(str.append(" ").append(unit));
}

QVariant TransactionTableModel::txStatusDecoration(const TransactionRecord* wtx) const
{
    QString theme = GUIUtil::getThemeName();
    switch (wtx->status.status) {
    case TransactionStatus::OpenUntilBlock:
    case TransactionStatus::OpenUntilDate:
        return COLOR_TX_STATUS_OPENUNTILDATE;
    case TransactionStatus::Offline:
        return COLOR_TX_STATUS_OFFLINE;
    case TransactionStatus::Unconfirmed:
        return QIcon(":/icons/" + theme + "/transaction_0");
    case TransactionStatus::Abandoned:
        return QIcon(":/icons/" + theme + "/transaction_abandoned");
    case TransactionStatus::Confirming:
        switch (wtx->status.depth) {
        case 1:
            return QIcon(":/icons/" + theme + "/transaction_1");
        case 2:
            return QIcon(":/icons/" + theme + "/transaction_2");
        case 3:
            return QIcon(":/icons/" + theme + "/transaction_3");
        case 4:
            return QIcon(":/icons/" + theme + "/transaction_4");
        default:
            return QIcon(":/icons/" + theme + "/transaction_5");
        };
    case TransactionStatus::Confirmed:
        return QIcon(":/icons/" + theme + "/transaction_confirmed");
    case TransactionStatus::Conflicted:
        return QIcon(":/icons/" + theme + "/transaction_conflicted");
    case TransactionStatus::Immature: {
        int total = wtx->status.depth + wtx->status.matures_in;
        int part = (wtx->status.depth * 4 / total) + 1;
        return QIcon(QString(":/icons/" + theme + "/transaction_%1").arg(part));
    }
    case TransactionStatus::MaturesWarning:
    case TransactionStatus::NotAccepted:
        return QIcon(":/icons/" + theme + "/transaction_0");
    default:
        return COLOR_BLACK;
    }
}

QVariant TransactionTableModel::txWatchonlyDecoration(const TransactionRecord* wtx) const
{
    QString theme = GUIUtil::getThemeName();
    if (wtx->involvesWatchAddress)
        return QIcon(":/icons/" + theme + "/eye");
    else
        return QVariant();
}

QString TransactionTableModel::formatTooltip(const TransactionRecord* rec) const
{
    QString tooltip = formatTxStatus(rec) + QString("\n") + formatTxType(rec);
    if (rec->type == TransactionRecord::RecvFromOther || rec->type == TransactionRecord::SendToOther ||
        rec->type == TransactionRecord::SendToAddress || rec->type == TransactionRecord::RecvWithAddress) {
        tooltip += QString(" ") + formatTxToAddress(rec, true);
    }
    return tooltip;
}

QVariant TransactionTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();
    TransactionRecord* rec = static_cast<TransactionRecord*>(index.internalPointer());

    if (showType == SHOW_LOCKED_TX && !rec->bLocked)
        return QVariant();

    switch (role) {
    case RawDecorationRole:
        switch (index.column()) {
        case TransactionColumnStatus:
            return txStatusDecoration(rec);
        case TransactionColumnWatchonly:
            return txWatchonlyDecoration(rec);
        case TransactionColumnToAddress:
            return txAddressDecoration(rec);
        }
        break;
    case Qt::DecorationRole: {
        return qvariant_cast<QIcon>(index.data(RawDecorationRole));
    }
    case Qt::DisplayRole: {
        switch (index.column()) {
        case TransactionColumnDate:
            return formatTxDate(rec);
        case TransactionColumnType: {
            QString result = formatTxType(rec);
            return result;
        }
        case TransactionColumnToAddress:
            return formatTxToAddress(rec, false);
        case TransactionColumnAssetsName:
            return formatAssetsName(rec);
        case TransactionColumnAmount: {
            if (rec->bGetCandy)
                return formatCandyAmount(rec, true, BitcoinUnits::separatorAlways);
            return formatTxAmount(rec, true, BitcoinUnits::separatorAlways);
        }
        }
        break;
    }
    case Qt::EditRole: {
        // Edit role is used for sorting, so return the unformatted values
        switch (index.column()) {
        case TransactionColumnStatus:
            return QString::fromStdString(rec->status.sortKey);
        case TransactionColumnDate:
            return rec->time;
        case TransactionColumnType:
            return formatTxType(rec);
        case TransactionColumnWatchonly:
            return (rec->involvesWatchAddress ? 1 : 0);
        case TransactionColumnToAddress:
            return formatTxToAddress(rec, true);
        case TransactionColumnAssetsName:
            return formatAssetsName(rec);
        case TransactionColumnAmount:
            if (rec->bSAFETransaction)
                return qint64(rec->credit + rec->debit);
            else if (rec->bGetCandy)
                return qint64(rec->getCandyData.nAmount);
            return qint64(rec->assetCredit + rec->assetDebit);
        }
        break;
    }
    case Qt::ToolTipRole:
        return formatTooltip(rec);
    case Qt::TextAlignmentRole: {
        return column_alignments[index.column()];
    }
    case Qt::ForegroundRole: {
        // Use the "danger" color for abandoned transactions
        if (rec->status.status == TransactionStatus::Abandoned) {
            return COLOR_TX_STATUS_DANGER;
        }
        // Non-confirmed (but not immature) as transactions are grey
        if (!rec->status.countsForBalance && rec->status.status != TransactionStatus::Immature) {
            return COLOR_UNCONFIRMED;
        }
        if (index.column() == TransactionColumnAmount && (rec->credit + rec->debit) < 0) {
            return COLOR_NEGATIVE;
        }
        if (index.column() == TransactionColumnToAddress) {
            return addressColor(rec);
        }
        break;
    }
    case TypeRole:
        return rec->type;
    case DateRole:
        return formatTxDate(rec);
    case WatchonlyRole:
        return rec->involvesWatchAddress;
    case WatchonlyDecorationRole:
        return txWatchonlyDecoration(rec);
    case LongDescriptionRole:
        return priv->describe(rec, walletModel->getOptionsModel()->getDisplayUnit(), !rec->bSAFETransaction);
    case AddressRole:
        return QString::fromStdString(rec->address);
    case LabelRole:
        return walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(rec->address));
    case AmountRole:
        return qint64(rec->credit + rec->debit);
    case AssetsAmountRole:
        return qint64(rec->assetCredit + rec->assetDebit);
    case AmountUnitRole: {
        if (rec->bSAFETransaction)
            return QString("SAFE");
        return QString::fromStdString(rec->assetsData.strAssetUnit);
    }
    case SAFERole:
        return rec->bSAFETransaction;
    case AssetsDecimalsRole:
        return rec->assetsData.nDecimals;
    case AssetsNameRole:
        return formatAssetsName(rec);
    case TxIDRole:
        return rec->getTxID();
    case TxHashRole:
        return QString::fromStdString(rec->hash.ToString());
    case TxHexRole:
        return priv->getTxHex(rec);
    case TxPlainTextRole: {
        QString details;
        QString txLabel = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(rec->address));

        details.append(formatTxDate(rec));
        details.append(" ");
        details.append(formatTxStatus(rec));
        details.append(". ");
        if (!formatTxType(rec).isEmpty()) {
            details.append(formatTxType(rec));
            details.append(" ");
        }
        if (!rec->address.empty()) {
            if (txLabel.isEmpty())
                details.append(tr("(no label)") + " ");
            else {
                details.append("(");
                details.append(txLabel);
                details.append(") ");
            }
            details.append(QString::fromStdString(rec->address));
            details.append(" ");
        }
        details.append(formatTxAmount(rec, false, BitcoinUnits::separatorNever));
        return details;
    }
    case ConfirmedRole:
        return rec->status.countsForBalance;
    case FormattedAmountRole: {
        // Used for copy/export, so don't include separators
        return formatTxAmount(rec, false, BitcoinUnits::separatorNever);
    }
    case StatusRole:
        return rec->status.status;
    case LockedMonthRole:
        return formatLockedMonth(rec);
    case UnlockedHeightRole:
        return formatUnlockedHeight(rec);
    case LockedStatusRole:
        return formatLockedStatus(rec);
    case Qt::FontRole: {
        QFont font;
        font.setPixelSize(12);
        return font;
    }
    }
    return QVariant();
}

QVariant TransactionTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal) {
        if (role == Qt::DisplayRole) {
            if (section >= 0 && section < columns.size()) {
                return columns[section];
            }
        } else if (role == Qt::TextAlignmentRole) {
            if (section >= 0 && section < sizeof(column_alignments) / sizeof(int)) {
                return column_alignments[section];
            }
        } else if (role == Qt::ToolTipRole) {
            switch (section) {
            case TransactionColumnStatus:
                return tr("Transaction status. Hover over this field to show number of confirmations.");
            case TransactionColumnDate:
                return tr("Date and time that the transaction was received.");
            case TransactionColumnType:
                return tr("Type of transaction.");
            case TransactionColumnWatchonly:
                return tr("Whether or not a watch-only address is involved in this transaction.");
            case TransactionColumnToAddress:
                return tr("User-defined intent/purpose of the transaction.");
            case TransactionColumnAmount:
                return tr("Amount removed from or added to balance.");
            }
        }
    }
    return QVariant();
}

QModelIndex TransactionTableModel::index(int row, int column, const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    TransactionRecord* data = priv->index(row);
    if (data) {
        return createIndex(row, column, priv->index(row));
    }
    return QModelIndex();
}

void TransactionTableModel::updateDisplayUnit()
{
    // emit dataChanged to update Amount column with the current unit
    if (columnAmount < 0) {
        return;
    }
    updateAmountColumnTitle();
    Q_EMIT dataChanged(index(0, columnAmount), index(priv->size() - 1, columnAmount));
}

void TransactionTableModel::beginRefresh()
{
	beginResetModel();
}

void TransactionTableModel::endRefresh()
{
	endResetModel();
}

void TransactionTableModel::insertTransaction(const TransactionRecord &tr)
{
	if (priv != NULL)
	{
		priv->insertTransaction(tr);
	}
}

void TransactionTableModel::clearData()
{
	if (priv != NULL)
	{
		priv->clearData();
	}
}
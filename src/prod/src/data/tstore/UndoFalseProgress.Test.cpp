// ------------------------------------------------------------
// Copyright (c) Microsoft Corporation.  All rights reserved.
// Licensed under the MIT License (MIT). See License.txt in the repo root for license information.
// ------------------------------------------------------------

#include "stdafx.h"

#include <boost/test/unit_test.hpp>
#include "Common/boost-taef.h"

namespace TStoreTests
{
    using namespace ktl;
    using namespace Data::TStore;

    class UndoFalseProgressTest : public TStoreTestBase<int, int, IntComparer, TestStateSerializer<int>, TestStateSerializer<int>>
    {
    public:
        UndoFalseProgressTest()
        {
            Setup();
            NTSTATUS status = TestStateSerializer<int>::Create(GetAllocator(), serializerSPtr_);
            Diagnostics::Validate(status);
        }

        ~UndoFalseProgressTest()
        {
            serializerSPtr_ = nullptr;
            Cleanup();
        }

        VersionedItem<int>::SPtr CreateInsertedVersionedItem(
            __in int value,
            __in LONG64 versionSequenceNumber)
        {
            InsertedVersionedItem<int>::SPtr itemSPtr;
            InsertedVersionedItem<int>::Create(GetAllocator(), itemSPtr);

            itemSPtr->InitializeOnApply(versionSequenceNumber, value);
            itemSPtr->SetValueSize(sizeof(int));

            return itemSPtr.DownCast<VersionedItem<int>>();
        }

        VersionedItem<int>::SPtr CreatedUpdatedVersionedItem(
            __in int value,
            __in LONG64 versionSequenceNumber)
        {
            UpdatedVersionedItem<int>::SPtr itemSPtr;
            UpdatedVersionedItem<int>::Create(GetAllocator(), itemSPtr);

            itemSPtr->InitializeOnApply(versionSequenceNumber, value);
            itemSPtr->SetValueSize(sizeof(int));

            return itemSPtr.DownCast<VersionedItem<int>>();
        }

        VersionedItem<int>::SPtr CreateDeletedVersionedItem(__in LONG64 versionSequenceNumber)
        {
            DeletedVersionedItem<int>::SPtr itemSPtr;
            DeletedVersionedItem<int>::Create(GetAllocator(), itemSPtr);
            itemSPtr->InitializeOnApply(versionSequenceNumber);
            return itemSPtr.DownCast<VersionedItem<int>>();
        }

        OperationData::SPtr GetBytes(__in int& value)
        {
            Utilities::BinaryWriter binaryWriter(GetAllocator());
            serializerSPtr_->Write(value, binaryWriter);

            OperationData::SPtr operationDataSPtr = OperationData::Create(this->GetAllocator());
            operationDataSPtr->Append(*binaryWriter.GetBuffer(0));

            return operationDataSPtr;
        }

        void SecondaryUndoFalseProgress(
            __in LONG64 sequenceNumber,
            __in StoreModificationType::Enum operationType, 
            __in int key)
        {
            KAllocator& allocator = GetAllocator();

            Transaction::SPtr tx = CreateReplicatorTransaction();
            Transaction::CSPtr txnCSPtr = tx.RawPtr();
            tx->CommitSequenceNumber = sequenceNumber;

            OperationData::SPtr keyBytes = GetBytes(key);

            OperationData::CSPtr metadataCSPtr = nullptr;

            KSharedPtr<MetadataOperationDataK<int> const> metadataKCSPtr = nullptr;
            NTSTATUS status = MetadataOperationDataK<int>::Create(
                key,
                Constants::SerializedVersion,
                operationType,
                txnCSPtr->TransactionId,
                keyBytes,
                allocator,
                metadataKCSPtr);
            Diagnostics::Validate(status);
            metadataCSPtr = static_cast<const OperationData* const>(metadataKCSPtr.RawPtr());

            auto operationContext = SyncAwait(Store->ApplyAsync(
                sequenceNumber,
                *txnCSPtr,
                ApplyContext::SecondaryFalseProgress,
                metadataCSPtr.RawPtr(),
                nullptr));

            if (operationContext != nullptr)
            {
                Store->Unlock(*operationContext);
            }
        }

        void UndoFalseProgressAdd(__in LONG64 sequenceNumber, __in int key)
        {
            SecondaryUndoFalseProgress(sequenceNumber, StoreModificationType::Enum::Add, key);
        }

        void UndoFalseProgressUpdate(__in LONG64 sequenceNumber, __in int key)
        {
            SecondaryUndoFalseProgress(sequenceNumber, StoreModificationType::Enum::Update, key);
        }

        void UndoFalseProgressRemove(__in LONG64 sequenceNumber, __in int key)
        {
            SecondaryUndoFalseProgress(sequenceNumber, StoreModificationType::Enum::Remove, key);
        }

        static ULONG IntHashFunc(__in const int & key)
        {
            return key;
        }

        StoreTraceComponent::SPtr CreateTraceComponent()
        {
            KGuid guid;
            guid.CreateNew();
            ::FABRIC_REPLICA_ID replicaId = 1;
            int stateProviderId = 1;
            
            StoreTraceComponent::SPtr traceComponent = nullptr;
            NTSTATUS status = StoreTraceComponent::Create(guid, replicaId, stateProviderId, GetAllocator(), traceComponent);
            CODING_ERROR_ASSERT(NT_SUCCESS(status));

            return traceComponent;
        }

        Common::CommonConfig config; // load the config object as its needed for the tracing to work

    private:
        KtlSystem* ktlSystem_;
        TestStateSerializer<int>::SPtr serializerSPtr_;
    };

    BOOST_FIXTURE_TEST_SUITE(UndoFalseProgressTestSuite, UndoFalseProgressTest)

    BOOST_AUTO_TEST_CASE(DifferentialStoreComponent_UndoAdd_ShouldSucceed)
    {
        int key = 5;
        int value = 2;

        auto keyComparer = Store->KeyComparerSPtr;
        ConsolidationManager<int, int>::SPtr consolidationManagerSPtr = Store->ConsolidationManagerSPtr;

        DifferentialStoreComponent<int, int>::SPtr differentialComponentSPtr = nullptr;
        DifferentialStoreComponent<int, int>::Create(UndoFalseProgressTest::IntHashFunc, *Replicator, *Store->SnapshotContainerSPtr, 0, *keyComparer, *CreateTraceComponent(), GetAllocator(), differentialComponentSPtr);

        LONG64 sequenceNumber = 0;

        auto initialAdd = CreateInsertedVersionedItem(value, sequenceNumber);
        differentialComponentSPtr->Add(key, *initialAdd, *consolidationManagerSPtr);
        
        auto versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr == nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetValue() == value);

        // Undo the add, the key should be removed
        differentialComponentSPtr->UndoFalseProgress(key, sequenceNumber, StoreModificationType::Enum::Add);

        versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions == nullptr);
    }

    BOOST_AUTO_TEST_CASE(DifferentialStoreComponent_UndoUpdate_AfterAdd_ShouldSucceed)
    {
        int key = 5;
        int originalValue = 2;
        int updatedValue = 6;

        auto keyComparer = Store->KeyComparerSPtr;
        ConsolidationManager<int, int>::SPtr consolidationManagerSPtr = Store->ConsolidationManagerSPtr;

        DifferentialStoreComponent<int, int>::SPtr differentialComponentSPtr = nullptr;
        DifferentialStoreComponent<int, int>::Create(UndoFalseProgressTest::IntHashFunc, *Replicator, *Store->SnapshotContainerSPtr, 0, *keyComparer, *CreateTraceComponent(), GetAllocator(), differentialComponentSPtr);

        LONG64 sequenceNumber = 0;

        auto initialAdd = CreateInsertedVersionedItem(originalValue, ++sequenceNumber);
        differentialComponentSPtr->Add(key, *initialAdd, *consolidationManagerSPtr);

        auto initialUpdate = CreatedUpdatedVersionedItem(updatedValue, ++sequenceNumber);
        differentialComponentSPtr->Add(key, *initialUpdate, *consolidationManagerSPtr);
        
        auto versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr->GetValue() == originalValue);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr->GetRecordKind() == RecordKind::InsertedVersion);

        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetValue() == updatedValue);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetRecordKind() == RecordKind::UpdatedVersion);

        // Undo the update, the key should be back to "inserted"
        differentialComponentSPtr->UndoFalseProgress(key, sequenceNumber--, StoreModificationType::Enum::Update);
        
        versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions != nullptr);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr == nullptr);

        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetValue() == originalValue);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetRecordKind() == RecordKind::InsertedVersion);
    }

    BOOST_AUTO_TEST_CASE(DifferentialStoreComponent_UndoUpdate_AfterUpdate_ShouldSucceed)
    {
        int key = 5;
        int originalValue = 2;
        int updatedValue = 6;
        int reupdatedValue = 9;

        ConsolidationManager<int, int>::SPtr consolidationManagerSPtr = Store->ConsolidationManagerSPtr;

        auto keyComparer = Store->KeyComparerSPtr;
        DifferentialStoreComponent<int, int>::SPtr differentialComponentSPtr = nullptr;
        DifferentialStoreComponent<int, int>::Create(UndoFalseProgressTest::IntHashFunc, *Replicator, *Store->SnapshotContainerSPtr, 0, *keyComparer, *CreateTraceComponent(), GetAllocator(), differentialComponentSPtr);

        LONG64 sequenceNumber = 0;

        auto initialAdd = CreateInsertedVersionedItem(originalValue, ++sequenceNumber);
        differentialComponentSPtr->Add(key, *initialAdd, *consolidationManagerSPtr);

        auto initialUpdate = CreatedUpdatedVersionedItem(updatedValue, ++sequenceNumber);
        differentialComponentSPtr->Add(key, *initialUpdate, *consolidationManagerSPtr);

        auto updateToUndo = CreatedUpdatedVersionedItem(reupdatedValue, ++sequenceNumber);
        differentialComponentSPtr->Add(key, *updateToUndo, *consolidationManagerSPtr);
        
        auto versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr->GetValue() == updatedValue);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr->GetRecordKind() == RecordKind::UpdatedVersion);

        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetValue() == reupdatedValue);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetRecordKind() == RecordKind::UpdatedVersion);

        // Undo the update, the key should be back to the previous value
        differentialComponentSPtr->UndoFalseProgress(key, sequenceNumber--, StoreModificationType::Enum::Update);
        
        versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions != nullptr);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr == nullptr);

        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetValue() == updatedValue);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetRecordKind() == RecordKind::UpdatedVersion);
    }

    BOOST_AUTO_TEST_CASE(DifferentialStoreComponent_UndoRemove_AfterAdd_ShouldSucceed)
    {
        int key = 5;
        int value = 2;

        ConsolidationManager<int, int>::SPtr consolidationManagerSPtr = Store->ConsolidationManagerSPtr;

        auto keyComparer = Store->KeyComparerSPtr;
        DifferentialStoreComponent<int, int>::SPtr differentialComponentSPtr = nullptr;
        DifferentialStoreComponent<int, int>::Create(UndoFalseProgressTest::IntHashFunc, *Replicator, *Store->SnapshotContainerSPtr, 0, *keyComparer, *CreateTraceComponent(), GetAllocator(), differentialComponentSPtr);

        LONG64 sequenceNumber = 0;

        auto initialAdd = CreateInsertedVersionedItem(value, ++sequenceNumber);
        differentialComponentSPtr->Add(key, *initialAdd, *consolidationManagerSPtr);

        auto remove = CreateDeletedVersionedItem(++sequenceNumber);
        differentialComponentSPtr->Add(key, *remove, *consolidationManagerSPtr);

        auto versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr->GetValue() == value);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr->GetRecordKind() == RecordKind::InsertedVersion);

        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetRecordKind() == RecordKind::DeletedVersion);

        // Undo the update, the key should be back to the initial value
        differentialComponentSPtr->UndoFalseProgress(key, sequenceNumber--, StoreModificationType::Enum::Remove);
        
        versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions != nullptr);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr == nullptr);

        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetValue() == value);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetRecordKind() == RecordKind::InsertedVersion);
    }

    BOOST_AUTO_TEST_CASE(DiffirentialStoreComponent_Add_Update_UndoAdd_ShouldSucceed)
    {
        int key = 5;
        int originalValue = 2;
        int updatedValue = 6;

        ConsolidationManager<int, int>::SPtr consolidationManagerSPtr = Store->ConsolidationManagerSPtr;

        auto keyComparer = Store->KeyComparerSPtr;
        DifferentialStoreComponent<int, int>::SPtr differentialComponentSPtr = nullptr;
        DifferentialStoreComponent<int, int>::Create(UndoFalseProgressTest::IntHashFunc, *Replicator, *Store->SnapshotContainerSPtr, 0, *keyComparer, *CreateTraceComponent(), GetAllocator(), differentialComponentSPtr);

        LONG64 sequenceNumber = 0;

        auto initialAdd = CreateInsertedVersionedItem(originalValue, ++sequenceNumber);
        differentialComponentSPtr->Add(key, *initialAdd, *consolidationManagerSPtr);

        LONG64 previousVersionSequenceNumber = sequenceNumber;

        auto update = CreatedUpdatedVersionedItem(updatedValue, ++sequenceNumber);
        differentialComponentSPtr->Add(key, *update, *consolidationManagerSPtr);
        
        auto versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr->GetValue() == originalValue);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr->GetRecordKind() == RecordKind::InsertedVersion);

        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetValue() == updatedValue);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetRecordKind() == RecordKind::UpdatedVersion);
        
        // Undo the add, the update should still be current
        differentialComponentSPtr->UndoFalseProgress(key, previousVersionSequenceNumber, StoreModificationType::Enum::Add);
        
        versions = differentialComponentSPtr->ReadVersions(key);

        CODING_ERROR_ASSERT(versions != nullptr);
        CODING_ERROR_ASSERT(versions->PreviousVersionSPtr == nullptr);

        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr != nullptr);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetValue() == updatedValue);
        CODING_ERROR_ASSERT(versions->CurrentVersionSPtr->GetRecordKind() == RecordKind::UpdatedVersion);
    }

    BOOST_AUTO_TEST_CASE(UndoFalseProgressAdd_ShouldSucceed)
    {
        int key = 5;
        int value = 2;

        LONG64 undoSequenceNumber = -1;
        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->AddAsync(*txn->StoreTransactionSPtr, key, value, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
            undoSequenceNumber = txn->TransactionSPtr->CommitSequenceNumber;
        }
        SyncAwait(VerifyKeyExistsAsync(*Store, key, -1, value));
        UndoFalseProgressAdd(undoSequenceNumber, key);
        SyncAwait(VerifyKeyDoesNotExistAsync(*Store, key));
    }

    BOOST_AUTO_TEST_CASE(UndoFalseProgressUpdate_ShouldSucceed)
    {
        int key = 5;
        int value = 2;
        int updatedValue = 8;

        LONG64 undoSequenceNumber = -1;

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->AddAsync(*txn->StoreTransactionSPtr, key, value, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
        }

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->ConditionalUpdateAsync(*txn->StoreTransactionSPtr, key, updatedValue, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
            undoSequenceNumber = txn->TransactionSPtr->CommitSequenceNumber;
        }

        SyncAwait(VerifyKeyExistsAsync(*Store, key, -1, updatedValue));
        UndoFalseProgressUpdate(undoSequenceNumber, key);
        SyncAwait(VerifyKeyExistsAsync(*Store, key, -1, value));
    }

    BOOST_AUTO_TEST_CASE(UndoFalseProgressUpdate_AfterUpdate_ShouldSucceed)
    {
        int key = 5;
        int value = 2;
        int update1Value = 8;
        int update2Value = 10;

        LONG64 undoSequenceNumber = -1;

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->AddAsync(*txn->StoreTransactionSPtr, key, value, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
        }

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->ConditionalUpdateAsync(*txn->StoreTransactionSPtr, key, update1Value, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
        }

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->ConditionalUpdateAsync(*txn->StoreTransactionSPtr, key, update2Value, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
            undoSequenceNumber = txn->TransactionSPtr->CommitSequenceNumber;
        }

        SyncAwait(VerifyKeyExistsAsync(*Store, key, -1, update2Value));
        UndoFalseProgressUpdate(undoSequenceNumber, key);
        SyncAwait(VerifyKeyExistsAsync(*Store, key, -1, update1Value));
    }

    BOOST_AUTO_TEST_CASE(UndoFalseProgressPreviousUpdate_AfterUpdate_ShouldSucceed)
    {
        int key = 5;
        int value = 2;
        int update1Value = 8;
        int update2Value = 10;

        LONG64 undoSequenceNumber = -1;

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->AddAsync(*txn->StoreTransactionSPtr, key, value, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
        }

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->ConditionalUpdateAsync(*txn->StoreTransactionSPtr, key, update1Value, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
            undoSequenceNumber = txn->TransactionSPtr->CommitSequenceNumber;
        }

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->ConditionalUpdateAsync(*txn->StoreTransactionSPtr, key, update2Value, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
        }

        SyncAwait(VerifyKeyExistsAsync(*Store, key, -1, update2Value));
        UndoFalseProgressUpdate(undoSequenceNumber, key);
        SyncAwait(VerifyKeyExistsAsync(*Store, key, -1, update2Value));
    }

    BOOST_AUTO_TEST_CASE(UndoFalseProgressRemove_ShouldSucceed)
    {
        int key = 5;
        int value = 2;

        LONG64 undoSequenceNumber = -1;

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->AddAsync(*txn->StoreTransactionSPtr, key, value, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
        }

        {
            auto txn = CreateWriteTransaction();
            SyncAwait(Store->ConditionalRemoveAsync(*txn->StoreTransactionSPtr, key, DefaultTimeout, CancellationToken::None));
            SyncAwait(txn->CommitAsync());
            undoSequenceNumber = txn->TransactionSPtr->CommitSequenceNumber;
        }

        SyncAwait(VerifyKeyDoesNotExistAsync(*Store, key));
        UndoFalseProgressRemove(undoSequenceNumber, key);
        SyncAwait(VerifyKeyExistsAsync(*Store, key, -1, value));
    }

    BOOST_AUTO_TEST_SUITE_END()
}

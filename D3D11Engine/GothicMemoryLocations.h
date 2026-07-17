#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

/** Memory location switch */
//#define BUILD_GOTHIC_1_08k
//#define BUILD_GOTHIC_2_6_fix

#define THISPTR_OFFSET(x) (reinterpret_cast<DWORD>(this) + (x))

namespace GothicPatching {
    constexpr size_t MaxPatchSize = 1024u * 1024u;

    struct PatchRecord {
        uintptr_t Address = 0;
        std::vector<unsigned char> Replacement;
        std::vector<unsigned char> Original;
        bool Applied = false;
    };

    struct TransactionAllocation {
        void* Address = nullptr;
        size_t Size = 0;
    };

    inline std::vector<PatchRecord> PendingPatches;
    inline std::vector<TransactionAllocation> PendingAllocations;
    inline LONG PatchStatus = ERROR_SUCCESS;
    inline uintptr_t FirstFailureAddress = 0;
    inline bool TransactionActive = false;
    inline bool TransactionCommitted = false;

    inline void RecordFailure( LONG error, uintptr_t address ) {
        if ( PatchStatus == ERROR_SUCCESS ) {
            PatchStatus = error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
            FirstFailureAddress = address;
        }
    }

    inline void ResetPatchStatus() {
        PatchStatus = ERROR_SUCCESS;
        FirstFailureAddress = 0;
    }

    inline bool IsExecutableProtection( DWORD protection ) {
        switch ( protection & 0xffu ) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    inline bool IsTransactionAllocationRange(
        uintptr_t address,
        size_t length ) {
        if ( address == 0 || length == 0
            || address > (std::numeric_limits<uintptr_t>::max)() - length ) {
            return false;
        }

        const uintptr_t end = address + length;
        for ( const auto& allocation : PendingAllocations ) {
            const uintptr_t allocationStart =
                reinterpret_cast<uintptr_t>(allocation.Address);
            if ( allocationStart == 0
                || allocation.Size
                    > (std::numeric_limits<uintptr_t>::max)()
                        - allocationStart ) {
                continue;
            }

            const uintptr_t allocationEnd = allocationStart + allocation.Size;
            if ( address >= allocationStart && end <= allocationEnd ) return true;
        }
        return false;
    }

    inline bool IsPatchRangeValid( uintptr_t address, size_t length ) {
        if ( address == 0 || length == 0 || length > MaxPatchSize
            || address > (std::numeric_limits<uintptr_t>::max)() - length ) {
            return false;
        }

        MEMORY_BASIC_INFORMATION memoryInfo{};
        if ( VirtualQuery(
                reinterpret_cast<const void*>(address), &memoryInfo,
                sizeof( memoryInfo ) ) != sizeof( memoryInfo )
            || memoryInfo.State != MEM_COMMIT
            || (memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ) {
            return false;
        }

        const uintptr_t regionStart =
            reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
        if ( memoryInfo.RegionSize
            > (std::numeric_limits<uintptr_t>::max)() - regionStart ) {
            return false;
        }
        const uintptr_t regionEnd = regionStart + memoryInfo.RegionSize;
        const uintptr_t patchEnd = address + length;
        if ( address < regionStart || patchEnd > regionEnd ) return false;

        if ( IsTransactionAllocationRange( address, length ) ) return true;
        if ( memoryInfo.Type == MEM_IMAGE ) {
            return memoryInfo.AllocationBase == GetModuleHandleW( nullptr );
        }
        return memoryInfo.Type == MEM_PRIVATE
            && IsExecutableProtection( memoryInfo.Protect );
    }

    inline LONG WriteMemoryImmediate(
        uintptr_t address,
        const void* data,
        size_t length ) {
        if ( !data || !IsPatchRangeValid( address, length ) ) {
            return ERROR_INVALID_ADDRESS;
        }

        const bool transactionAllocation =
            IsTransactionAllocationRange( address, length );
        MEMORY_BASIC_INFORMATION memoryInfo{};
        if ( VirtualQuery(
                reinterpret_cast<const void*>(address), &memoryInfo,
                sizeof( memoryInfo ) ) != sizeof( memoryInfo ) ) {
            return ERROR_INVALID_ADDRESS;
        }

        const DWORD writableProtection =
            transactionAllocation || !IsExecutableProtection( memoryInfo.Protect )
                ? PAGE_READWRITE
                : PAGE_EXECUTE_READWRITE;
        DWORD previousProtection = 0;
        if ( !VirtualProtect(
                reinterpret_cast<void*>(address), length,
                writableProtection, &previousProtection ) ) {
            const DWORD error = GetLastError();
            return error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error;
        }

        std::memcpy( reinterpret_cast<void*>(address), data, length );

        DWORD ignoredProtection = 0;
        const BOOL protectionRestored = VirtualProtect(
            reinterpret_cast<void*>(address), length,
            previousProtection, &ignoredProtection );
        const BOOL cacheFlushed = FlushInstructionCache(
            GetCurrentProcess(), reinterpret_cast<const void*>(address), length );
        if ( !protectionRestored || !cacheFlushed ) {
            const DWORD error = GetLastError();
            return error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error;
        }
        return ERROR_SUCCESS;
    }

    inline void ReleaseTransactionAllocations() {
        for ( const auto& allocation : PendingAllocations ) {
            if ( allocation.Address
                && !VirtualFree( allocation.Address, 0, MEM_RELEASE ) ) {
                const DWORD error = GetLastError();
                RecordFailure(
                    error == ERROR_SUCCESS ? ERROR_INVALID_ADDRESS : error,
                    reinterpret_cast<uintptr_t>(allocation.Address) );
            }
        }
        PendingAllocations.clear();
    }

    inline void* AllocateTrampoline( size_t size ) {
        if ( !TransactionActive || TransactionCommitted
            || size == 0 || size > MaxPatchSize ) {
            RecordFailure( ERROR_INVALID_PARAMETER, 0 );
            return nullptr;
        }

        void* allocation = VirtualAlloc(
            nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        if ( !allocation ) {
            const DWORD error = GetLastError();
            RecordFailure(
                error == ERROR_SUCCESS ? ERROR_NOT_ENOUGH_MEMORY : error, 0 );
            return nullptr;
        }

        try {
            PendingAllocations.push_back( { allocation, size } );
        } catch ( const std::bad_alloc& ) {
            VirtualFree( allocation, 0, MEM_RELEASE );
            RecordFailure( ERROR_NOT_ENOUGH_MEMORY, 0 );
            return nullptr;
        }
        return allocation;
    }

    inline bool ProtectTransactionAllocations() {
        for ( const auto& allocation : PendingAllocations ) {
            DWORD previousProtection = 0;
            if ( !VirtualProtect(
                    allocation.Address, allocation.Size,
                    PAGE_EXECUTE_READ, &previousProtection )
                || !FlushInstructionCache(
                    GetCurrentProcess(), allocation.Address, allocation.Size ) ) {
                const DWORD error = GetLastError();
                RecordFailure(
                    error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error,
                    reinterpret_cast<uintptr_t>(allocation.Address) );
                return false;
            }
        }
        return true;
    }

    inline bool BeginPatchTransaction() {
        if ( TransactionActive || TransactionCommitted ) {
            RecordFailure( ERROR_BUSY, 0 );
            return false;
        }
        PendingPatches.clear();
        if ( !PendingAllocations.empty() ) ReleaseTransactionAllocations();
        ResetPatchStatus();
        TransactionActive = true;
        return true;
    }

    inline bool QueueOrApply(
        uintptr_t address,
        const void* data,
        size_t length ) {
        if ( !data || !IsPatchRangeValid( address, length ) ) {
            RecordFailure( ERROR_INVALID_ADDRESS, address );
            return false;
        }
        if ( TransactionCommitted ) {
            RecordFailure( ERROR_BUSY, address );
            return false;
        }

        if ( TransactionActive ) {
            try {
                PatchRecord record;
                record.Address = address;
                const auto* bytes = static_cast<const unsigned char*>(data);
                record.Replacement.assign( bytes, bytes + length );
                PendingPatches.push_back( std::move( record ) );
                return true;
            } catch ( const std::bad_alloc& ) {
                RecordFailure( ERROR_NOT_ENOUGH_MEMORY, address );
                return false;
            }
        }

        const LONG result = WriteMemoryImmediate( address, data, length );
        if ( result != ERROR_SUCCESS ) RecordFailure( result, address );
        return result == ERROR_SUCCESS;
    }

    inline void RollbackAppliedPatches() {
        for ( auto patch = PendingPatches.rbegin();
            patch != PendingPatches.rend(); ++patch ) {
            if ( !patch->Applied || patch->Original.empty() ) continue;
            const LONG result = WriteMemoryImmediate(
                patch->Address, patch->Original.data(), patch->Original.size() );
            if ( result != ERROR_SUCCESS ) {
                RecordFailure( result, patch->Address );
            }
            patch->Applied = false;
        }
    }

    inline bool CommitPatchTransaction() {
        if ( !TransactionActive || TransactionCommitted ) {
            RecordFailure( ERROR_INVALID_FUNCTION, 0 );
            return false;
        }
        if ( PatchStatus != ERROR_SUCCESS ) {
            TransactionActive = false;
            PendingPatches.clear();
            ReleaseTransactionAllocations();
            return false;
        }

        for ( auto& patch : PendingPatches ) {
            if ( !IsPatchRangeValid(
                    patch.Address, patch.Replacement.size() ) ) {
                RecordFailure( ERROR_INVALID_ADDRESS, patch.Address );
                RollbackAppliedPatches();
                TransactionActive = false;
                PendingPatches.clear();
                ReleaseTransactionAllocations();
                return false;
            }

            try {
                patch.Original.resize( patch.Replacement.size() );
            } catch ( const std::bad_alloc& ) {
                RecordFailure( ERROR_NOT_ENOUGH_MEMORY, patch.Address );
                RollbackAppliedPatches();
                TransactionActive = false;
                PendingPatches.clear();
                ReleaseTransactionAllocations();
                return false;
            }

            std::memcpy(
                patch.Original.data(), reinterpret_cast<const void*>(patch.Address),
                patch.Original.size() );
            patch.Applied = true;
            const LONG result = WriteMemoryImmediate(
                patch.Address, patch.Replacement.data(), patch.Replacement.size() );
            if ( result != ERROR_SUCCESS ) {
                RecordFailure( result, patch.Address );
                RollbackAppliedPatches();
                TransactionActive = false;
                PendingPatches.clear();
                ReleaseTransactionAllocations();
                return false;
            }
        }

        if ( !ProtectTransactionAllocations() ) {
            RollbackAppliedPatches();
            TransactionActive = false;
            PendingPatches.clear();
            ReleaseTransactionAllocations();
            return false;
        }

        TransactionActive = false;
        TransactionCommitted = true;
        return true;
    }
    inline void RollbackPatchTransaction() {
        RollbackAppliedPatches();
        PendingPatches.clear();
        ReleaseTransactionAllocations();
        TransactionActive = false;
        TransactionCommitted = false;
    }

    inline void AbortPatchTransaction() {
        RollbackPatchTransaction();
    }

    inline void FinalizePatchTransaction() {
        PendingPatches.clear();
        PendingAllocations.clear();
        TransactionActive = false;
        TransactionCommitted = false;
    }

    inline LONG GetPatchStatus() {
        return PatchStatus;
    }

    inline uintptr_t GetFirstFailureAddress() {
        return FirstFailureAddress;
    }
}

template<typename T, size_t n>
static bool PatchAddr( unsigned int address, const T( &value )[n] ) {
    static_assert( sizeof( T ) == 1,
        "PatchAddr accepts byte arrays only; use PatchValue for typed values." );
    static_assert( n > 1, "PatchAddr requires at least one payload byte." );
    return GothicPatching::QueueOrApply(
        static_cast<uintptr_t>(address), value, n - 1 );
}

template<typename T>
static bool PatchValue( unsigned int address, const T& value ) {
    static_assert( std::is_trivially_copyable_v<T>,
        "PatchValue requires a trivially copyable value." );
    return GothicPatching::QueueOrApply(
        static_cast<uintptr_t>(address), &value, sizeof( value ) );
}

static bool PatchCall( unsigned int address, unsigned int function ) {
    if ( address == 0 || function == 0 ) {
        GothicPatching::RecordFailure( ERROR_INVALID_ADDRESS, address );
        return false;
    }

    std::array<unsigned char, 5> patch{};
    patch[0] = 0xe8;
    const DWORD displacement = function - address - 5u;
    std::memcpy( patch.data() + 1, &displacement, sizeof( displacement ) );
    return GothicPatching::QueueOrApply(
        static_cast<uintptr_t>(address), patch.data(), patch.size() );
}

static bool PatchJMP( unsigned int address, unsigned int destination ) {
    if ( address == 0 || destination == 0 ) {
        GothicPatching::RecordFailure( ERROR_INVALID_ADDRESS, address );
        return false;
    }

    std::array<unsigned char, 5> patch{};
    patch[0] = 0xe9;
    const DWORD displacement = destination - address - 5u;
    std::memcpy( patch.data() + 1, &displacement, sizeof( displacement ) );
    return GothicPatching::QueueOrApply(
        static_cast<uintptr_t>(address), patch.data(), patch.size() );
}

static bool PatchFillRange(
    unsigned int start,
    unsigned int endInclusive,
    unsigned char value ) {
    if ( endInclusive < start ) {
        GothicPatching::RecordFailure( ERROR_INVALID_PARAMETER, start );
        return false;
    }

    const size_t length =
        static_cast<size_t>(endInclusive) - static_cast<size_t>(start) + 1u;
    try {
        std::vector<unsigned char> bytes( length, value );
        return GothicPatching::QueueOrApply(
            static_cast<uintptr_t>(start), bytes.data(), bytes.size() );
    } catch ( const std::bad_alloc& ) {
        GothicPatching::RecordFailure( ERROR_NOT_ENOUGH_MEMORY, start );
        return false;
    }
}

#define INST_NOP 0x90
#define REPLACE_OP(addr, op) \
    do { PatchFillRange( static_cast<unsigned int>(addr), \
        static_cast<unsigned int>(addr), static_cast<unsigned char>(op) ); } while ( false )
#define REPLACE_RANGE(start, end_incl, op) \
    do { PatchFillRange( static_cast<unsigned int>(start), \
        static_cast<unsigned int>(end_incl), static_cast<unsigned char>(op) ); } while ( false )

#ifdef BUILD_GOTHIC_1_08k
#ifdef BUILD_1_12F
#include "GothicMemoryLocations1_12f.h"
#else
#include "GothicMemoryLocations1_08k.h"
#endif
#endif

#ifdef BUILD_GOTHIC_2_6_fix
#ifdef BUILD_SPACER
#include "GothicMemoryLocations2_6_fix_Spacer.h"
#else
#include "GothicMemoryLocations2_6_fix.h"
#endif
#endif

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeCounter.h"

/**
 * A pool for managing reusable message buffers.
 * This class is designed to provide efficient allocation
 * and deallocation of buffers to reduce memory allocation overhead.
 */
class CROWDYSDK_API FMessageBufferPool
{

public:
	/**
	 * Constructs an instance of FMessageBufferPool and pre-allocates a pool of message buffers.
	 * Initializes a queue with a specified number of buffers, each having a predefined capacity.
	 * Logs a warning to indicate successful initialization with the pool size and buffer capacity.
	 *
	 * @return A new instance of FMessageBufferPool with a pre-initialized pool of buffers.
	 */
	FMessageBufferPool();
	/**
	 * Destructor for the FMessageBufferPool class.
	 *
	 * Cleans up the buffer pool by dequeuing all buffers from the internal queue
	 * and releasing their memory. Ensures proper cleanup of dynamically allocated
	 * resources to avoid memory leaks.
	 */
	~FMessageBufferPool();

	/**
	 * Defines the default size (in bytes) for buffers allocated by the message buffer pool.
	 * DefaultBufferSize is used to preallocate memory for buffers in the pool, providing
	 * an initial capacity that can help optimize memory usage and avoid frequent reallocations.
	 *
	 * This value is utilized during the initialization of the buffer pool to set the reserved
	 * memory size for each buffer in the pool.
	 *
	 * The current default value is set to 8192 bytes.
	 */
	int32 DefaultBufferSize = 8192;

	/**
	 * Specifies the maximum number of message buffers that can be held in the pool.
	 * This value determines the upper limit for pre-allocated buffers to optimize
	 * memory usage and reduce runtime allocations.
	 *
	 * A higher value will increase memory usage but can improve performance in
	 * scenarios requiring frequent buffer allocation and release.
	 *
	 * Default value: 10000
	 */
	int32 MaxPoolSize = 10000;

	/**
	 * Retrieves a buffer from the pool, empties it, and decrements the internal buffer counter.
	 * If the pool is empty, returns a null pointer.
	 *
	 * @return A pointer to an empty buffer retrieved from the pool, or nullptr if the pool is empty.
	 */
	TArray<uint8>* GetBuffer();
	/**
	 * Releases a buffer previously allocated by the buffer pool. If the pool has not reached its maximum size,
	 * the buffer is returned to the pool for reuse; otherwise, the buffer is deallocated.
	 *
	 * @param Buffer Pointer to the buffer that needs to be released. If null, the method immediately returns.
	 */
	void ReleaseBuffer(TArray<uint8>* Buffer);

	/**
	 * A thread-safe queue used for managing reusable message buffers in the buffer pool.
	 *
	 * This queue is part of the implementation of the `FMessageBufferPool` class, which provides
	 * a pool of pre-allocated buffers to optimize memory management for messaging systems. The queue
	 * stores pointers to dynamically allocated `TArray<uint8>` objects, which serve as the buffers.
	 *
	 * The queue enables efficient enqueueing and dequeueing of buffers, ensuring that buffers are made
	 * available for reuse in a multi-threaded environment. Access to the queue is protected by a
	 * `FCriticalSection`, and the lifetime of the buffers is managed to avoid memory leaks by
	 * releasing unused buffers during pool destruction.
	 */
private:
    TQueue<TArray<uint8>*> BufferQueue;
	/**
	 * Synchronization primitive used to ensure thread-safe access to the buffer queue within
	 * the message buffer pool. It protects operations where multiple threads may concurrently
	 * interact with the `BufferQueue` and `BufferCounter`, such as acquiring or releasing buffers.
	 *
	 * This mutex is utilized by methods like `GetBuffer` and `ReleaseBuffer` to prevent data
	 * races and ensure consistency of shared resources.
	 */
	FCriticalSection BufferMutex;
	/**
	 * A thread-safe counter used to track the number of active buffers in the message buffer pool.
	 *
	 * This variable is an instance of FThreadSafeCounter and is utilized to maintain an accurate count
	 * of allocated and available buffers in the FMessageBufferPool class. It is incremented when a
	 * buffer is added to the pool and decremented when a buffer is dequeued for use.
	 *
	 * The counter helps enforce constraints on buffer allocation, ensuring the total number of buffers
	 * does not exceed `MaxPoolSize` as defined in the FMessageBufferPool class.
	 */
	FThreadSafeCounter BufferCounter;
};


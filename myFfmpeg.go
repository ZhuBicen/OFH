package main

import (
	"fmt"
	"io"
	"os"
	"time"
)

// bufferSize defines the size of the buffer used for reading and writing.
// A larger buffer might reduce system calls but increase latency for rate limiting.
// 4KB is a common and reasonable size.
const bufferSize = 4096 // bytes (4 KB)

func main() {
	// Define the default output FIFO path.
	const defaultOutputFifoPath = "/tmp/video_tunnel_in"

	var inputFilePath string
	var outputFifoPath string

	// Check the number of command-line arguments.
	if len(os.Args) == 2 {
		// Only input file path provided, use default for output FIFO.
		inputFilePath = os.Args[1]
		outputFifoPath = defaultOutputFifoPath
	} else if len(os.Args) == 3 {
		// Both input file and output FIFO paths provided.
		inputFilePath = os.Args[1]
		outputFifoPath = os.Args[2]
	} else {
		// Incorrect number of arguments.
		fmt.Printf("Usage: %s <input_file_path> [output_fifo_path]\n", os.Args[0])
		fmt.Printf("  If output_fifo_path is omitted, it defaults to '%s'\n", defaultOutputFifoPath)
		os.Exit(1)
	}

	// Define the target speed as specified by the user: 20252.5 kbits/s.
	// 1 kbit is defined as 1000 bits (standard SI prefix).
	// 1 byte is 8 bits.
	const targetKbitsPerSecond = 20252.5
	const targetBitsPerSecond = targetKbitsPerSecond * 1000
	const targetBytesPerSecond = targetBitsPerSecond / 8 // Convert bits/s to bytes/s

	// Open the input file for reading.
	inputFile, err := os.Open(inputFilePath)
	if err != nil {
		fmt.Printf("Error opening input file '%s': %v\n", inputFilePath, err)
		os.Exit(1)
	}
	defer inputFile.Close() // Ensure the input file is closed when main exits.

	// Open the output FIFO for writing.
	// os.O_WRONLY: Open the file write-only.
	// 0: Permissions are ignored for FIFOs as they are managed by the filesystem.
	// Note: Opening a FIFO for writing will block until a reader opens it on the other end.
	outputFifo, err := os.OpenFile(outputFifoPath, os.O_WRONLY, 0)
	if err != nil {
		fmt.Printf("Error opening output FIFO '%s': %v\n", outputFifoPath, err)
		os.Exit(1)
	}
	defer outputFifo.Close() // Ensure the output FIFO is closed when main exits.

	fmt.Printf("Starting transfer from '%s' to '%s'\n", inputFilePath, outputFifoPath)
	fmt.Printf("Target speed: %.2f kbits/s (approx. %.2f bytes/s)\n",
		targetKbitsPerSecond, targetBytesPerSecond)

	buffer := make([]byte, bufferSize) // Create a byte buffer for read/write operations.
	totalBytesWritten := int64(0)      // Keep track of the total bytes transferred.
	startTime := time.Now()            // Record the start time for speed calculation.

	// Main loop for reading from input and writing to output.
	for {
		// Read a chunk of data from the input file into the buffer.
		nRead, readErr := inputFile.Read(buffer)

		// If some bytes were read successfully.
		if nRead > 0 {
			// Write the read bytes to the output FIFO.
			nWritten, writeErr := outputFifo.Write(buffer[:nRead])
			if writeErr != nil {
				fmt.Printf("Error writing to FIFO: %v\n", writeErr)
				os.Exit(1)
			}
			// Check if all bytes read were successfully written.
			if nWritten != nRead {
				fmt.Printf("Warning: Bytes written (%d) mismatch bytes read (%d)\n", nWritten, nRead)
			}
			totalBytesWritten += int64(nWritten) // Update total bytes written.

			// --- Rate Limiting Logic ---
			// Calculate the actual time elapsed since the start of the transfer.
			elapsedTime := time.Since(startTime)

			// Calculate the desired time that *should* have elapsed to transfer
			// 'totalBytesWritten' at the 'targetBytesPerSecond' rate.
			// This conversion is crucial: (totalBytesWritten / targetBytesPerSecond) gives seconds.
			// Multiplying by float64(time.Second) converts it to a time.Duration.
			desiredElapsedTime := time.Duration(float64(totalBytesWritten) / targetBytesPerSecond * float64(time.Second))

			// If the actual elapsed time is less than the desired elapsed time,
			// it means we are sending data too fast.
			if elapsedTime < desiredElapsedTime {
				sleepDuration := desiredElapsedTime - elapsedTime // Calculate how long to sleep.
				time.Sleep(sleepDuration)                        // Pause execution for the calculated duration.
			}
		}

		// Check for end-of-file (EOF) from the input file.
		if readErr == io.EOF {
			fmt.Println("Finished reading input file.")
			break // Exit the loop when EOF is reached.
		}
		// Handle any other errors during reading.
		if readErr != nil {
			fmt.Printf("Error reading from input file: %v\n", readErr)
			os.Exit(1)
		}
	}

	// --- Final Statistics ---
	finalElapsedTime := time.Since(startTime) // Get the total time taken for the transfer.
	fmt.Printf("Total bytes written: %d bytes\n", totalBytesWritten)
	fmt.Printf("Total time taken: %v\n", finalElapsedTime.Round(time.Millisecond)) // Round for cleaner output

	// Calculate and display the actual average transfer speed.
	if finalElapsedTime > 0 {
		actualBytesPerSecond := float64(totalBytesWritten) / finalElapsedTime.Seconds()
		actualKbitsPerSecond := (actualBytesPerSecond * 8) / 1000 // Convert bytes/s to kbits/s
		fmt.Printf("Actual average speed: %.2f kbits/s (%.2f bytes/s)\n", actualKbitsPerSecond, actualBytesPerSecond)
	}
}

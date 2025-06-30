package main

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"os"
	"sync"
	"time"
)

const (
	defaultFifoPath   = "/tmp/video_tunnel_out"     // Default path to the Linux FIFO (named pipe)
	defaultFilePath   = "/tmp/video_tunnel_out.log" // Default path for the local output file
	retryInterval     = 5 * time.Second             // Interval to wait before retrying TCP connection
	statsPrintInterval = 10 * time.Second            // Interval to print statistics
	channelBufferSize  = 30000                        // Buffer size for each data channel (number of []byte slices)
	maxBytesPerMessage = 4096                        // Max bytes in a single message (fifo read chunk size)
)

// ChannelStats holds the statistics for data flowing through the channels.
type ChannelStats struct {
	mu sync.Mutex
	// Statistics for the TCP path
	tcpBufferedMessages uint64 // Current messages buffered in TCP channel
	tcpConnected        bool   // True if TCP connection is active, false otherwise
	// Statistics for the File path
	fileBufferedMessages uint64 // Current messages buffered in File channel
}

// fifoReader reads data from the specified FIFO and sends it to both data channels.
// It updates the provided ChannelStats and drops data if a channel is full.
func fifoReader(dataChannelToTCP, dataChannelToFile chan<- []byte, wg *sync.WaitGroup, stats *ChannelStats, fifoPath string) {
	defer wg.Done()

	fmt.Printf("Attempting to open FIFO: %s\n", fifoPath)
	file, err := os.OpenFile(fifoPath, os.O_RDONLY, 0600)
	if err != nil {
		fmt.Printf("Error opening FIFO %s: %v\n", fifoPath, err)
		return
	}
	defer file.Close()
	fmt.Printf("FIFO %s opened successfully. Waiting for data...\n", fifoPath)

	reader := bufio.NewReader(file)
	buffer := make([]byte, maxBytesPerMessage) // Read in 4KB chunks

	for {
		n, err := reader.Read(buffer)
		if err != nil {
			if err == io.EOF {
				fmt.Println("EOF encountered on FIFO, possibly writer disconnected. Re-opening or waiting...")
				time.Sleep(1 * time.Second) // Prevent busy-looping
				continue
			}
			fmt.Printf("Error reading from FIFO: %v\n", err)
			return // Exit goroutine on serious read error
		}
		if n > 0 {
			data := make([]byte, n)
			copy(data, buffer[:n])

			// Try to send to TCP channel
			select {
			case dataChannelToTCP <- data:
				stats.mu.Lock()
				stats.tcpBufferedMessages++
				stats.mu.Unlock()
			default:
				fmt.Printf("TCP channel buffer full, dropping %d bytes (message) for TCP. Consider increasing channelBufferSize.\n", n)
			}

			// Try to send to File channel
			select {
			case dataChannelToFile <- data:
				stats.mu.Lock()
				stats.fileBufferedMessages++
				stats.mu.Unlock()
			default:
				fmt.Printf("File channel buffer full, dropping %d bytes (message) for File. Consider increasing channelBufferSize.\n", n)
			}
		}
	}
}

// tcpWriter receives data from the dataChannelToTCP and sends it over a TCP connection.
// It handles connection retries if the connection is lost, and drops data from channel
// when disconnected to prevent the channel from filling up.
func tcpWriter(dataChannelToTCP <-chan []byte, wg *sync.WaitGroup, tcpAddr string, stats *ChannelStats) {
	defer wg.Done()

	var conn net.Conn
	var err error

	for {
		// Connection loop: Try to establish a TCP connection
		for conn == nil {
			fmt.Printf("Attempting to connect to TCP server: %s\n", tcpAddr)
			conn, err = net.Dial("tcp", tcpAddr)
			if err != nil {
				fmt.Printf("Error connecting to TCP server: %v. Retrying in %v...\n", err, retryInterval)
				// Set connected status to false if connection fails
				stats.mu.Lock()
				stats.tcpConnected = false
				stats.mu.Unlock()

				// While disconnected, actively drain the channel to prevent fifoReader from blocking
				select {
				case data := <-dataChannelToTCP:
					// Data received while disconnected, drop it
					stats.mu.Lock()
					if stats.tcpBufferedMessages > 0 { // Ensure not to decrement below zero
						stats.tcpBufferedMessages--
					}
					stats.mu.Unlock()
					fmt.Printf("TCP disconnected, dropping %d bytes (message) from TCP channel while attempting reconnect.\n", len(data))
					time.Sleep(100 * time.Millisecond) // Small delay to avoid busy-loop
				case <-time.After(retryInterval):
					// No data in channel, or no data received within timeout, wait for retry interval
				}
				continue // Go back to connection attempt
			}
			fmt.Printf("Successfully connected to TCP server: %s\n", tcpAddr)
			// Set connected status to true once connected
			stats.mu.Lock()
			stats.tcpConnected = true
			stats.mu.Unlock()
		}

		// Data writing loop: Only entered when connected
		select {
		case data, ok := <-dataChannelToTCP:
			if !ok {
				// Channel closed, exit goroutine
				fmt.Println("TCP data channel closed, exiting TCP writer.")
				if conn != nil {
					conn.Close()
				}
				// Set connected status to false on exit
				stats.mu.Lock()
				stats.tcpConnected = false
				stats.mu.Unlock()
				return
			}

			// Update buffered stats after receiving from channel (data is now out of the buffer)
			stats.mu.Lock()
			if stats.tcpBufferedMessages > 0 { // Ensure not to decrement below zero
				stats.tcpBufferedMessages--
			}
			stats.mu.Unlock()

			_, err := conn.Write(data)
			if err != nil {
				fmt.Printf("Error writing to TCP connection: %v. Connection lost. Retrying...\n", err)
				conn.Close() // Close the broken connection
				conn = nil   // Reset connection to trigger reconnect loop
				// Set connected status to false as connection is lost
				stats.mu.Lock()
				stats.tcpConnected = false
				stats.mu.Unlock()
				// The outer 'for' loop will automatically re-enter the connection loop
			} else {
				// fmt.Printf("Sent %d bytes to TCP\n", n) // Debug print
			}
		}
	}
}

// fileWriter receives data from the dataChannelToFile and appends it to a local file.
func fileWriter(dataChannelToFile <-chan []byte, wg *sync.WaitGroup, stats *ChannelStats, filePath string) {
	defer wg.Done()

	fmt.Printf("Attempting to open/create file for writing (and truncating if it exists): %s\n", filePath)
	// Open file in truncate mode, create if it doesn't exist, write-only permissions
	file, err := os.OpenFile(filePath, os.O_TRUNC|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		fmt.Printf("Error opening file %s: %v\n", filePath, err)
		return
	}
	defer file.Close()
	fmt.Printf("File %s opened successfully for appending.\n", filePath)

	writer := bufio.NewWriter(file) // Use a buffered writer for efficiency

	originVideoFilePath := "/home/systemci/Mission.Impossible.7.2023.2160p.HQ.WEB-DL.H265.DDP5.1.4Audios.mkv"
	originVideoFile, err := os.OpenFile(originVideoFilePath, os.O_RDONLY, 0644)
	if err == nil {
		defer originVideoFile.Close()
		fmt.Printf("Origin video file %s opened successfully for reading.\n", originVideoFilePath)
	} else {
		fmt.Printf("Error opening origin video file %s: %v\n", originVideoFilePath, err)
	}


	offset := uint64(0)
	for {
		select {
		case data, ok := <-dataChannelToFile:
			if !ok {
				// Channel closed, exit goroutine
				fmt.Println("File data channel closed, exiting file writer.")
				if err := writer.Flush(); err != nil { // Flush any buffered data before closing
					fmt.Printf("Error flushing file writer: %v\n", err)
				}
				return
			}

			originData := make([]byte, len(data))
			originVideoFile.Seek(int64(offset), io.SeekStart)
			originVideoFile.Read(originData) // Read data from origin video file at the current offset

			// compare data with originData
			if len(data) != len(originData) {
				fmt.Printf("Data length mismatch: received %d bytes, expected %d bytes at offset %d\n", len(data), len(originData), offset)
			} else {
				for i := 0; i < len(data); i++ {
					if data[i] != originData[i] {
						fmt.Printf("Data mismatch at byte %d: received %d, expected %d at offset %d\n", i, data[i], originData[i], offset)
						break // Only report the first mismatch
					}
				}
			}

			offset += uint64(len(data))
			// Update buffered stats after receiving from channel
			stats.mu.Lock()
			if stats.fileBufferedMessages > 0 { // Ensure not to decrement below zero
				stats.fileBufferedMessages--
			}
			stats.mu.Unlock()

			_, err := writer.Write(data)
			if err != nil {
				fmt.Printf("Error writing to file %s: %v\n", filePath, err)
				// In a real application, you might want more robust error handling,
				// like attempting to close and reopen the file or notify of persistent errors.
				// For now, we'll just log the error and continue, potentially dropping data.
			}

			// Flush occasionally or when data rate is low to ensure data is written to disk
			// For high-rate data, rely on buffered writer and flush on close or periodic timer
			// if stats.fileBufferedMessages == 0 { // Example: flush when buffer is empty
			// 	if err := writer.Flush(); err != nil {
			// 		fmt.Printf("Error flushing file writer: %v\n", err)
			// 	}
			// }
		}
	}
}

// statsPrinter periodically prints the collected channel statistics.
func statsPrinter(stats *ChannelStats, wg *sync.WaitGroup) {
	defer wg.Done()
	ticker := time.NewTicker(statsPrintInterval)
	defer ticker.Stop()

	for range ticker.C {
		stats.mu.Lock()
		fmt.Printf("--- Channel Statistics ---\n")
		fmt.Printf("  TCP Buffered Messages: %d\n", stats.tcpBufferedMessages)
		fmt.Printf("  TCP Connected: %t\n", stats.tcpConnected)
		fmt.Printf("  File Buffered Messages: %d\n", stats.fileBufferedMessages)
		fmt.Printf("--------------------------\n")
		stats.mu.Unlock()
	}
}

func main() {
	if len(os.Args) < 2 || len(os.Args) > 4 {
		fmt.Println("Usage: ./fifo_to_tcp <tcp_address> [fifo_path] [file_output_path]")
		fmt.Println("Example: ./fifo_to_tcp localhost:8080")
		fmt.Println("Example: ./fifo_to_tcp localhost:8080 /tmp/my_custom_fifo")
		fmt.Println("Example: ./fifo_to_tcp localhost:8080 /tmp/my_custom_fifo /var/log/my_data.log")
		os.Exit(1)
	}

	tcpAddr := os.Args[1] // Get TCP address from command-line argument
	fifoPath := defaultFifoPath // Initialize with default FIFO path
	filePath := defaultFilePath   // Initialize with default file path

	if len(os.Args) >= 3 {
		fifoPath = os.Args[2] // If provided, use the second argument as FIFO path
	}
	if len(os.Args) == 4 {
		filePath = os.Args[3] // If provided, use the third argument as file path
	}

	var wg sync.WaitGroup
	dataChannelToTCP := make(chan []byte, channelBufferSize)
	dataChannelToFile := make(chan []byte, channelBufferSize)
	stats := &ChannelStats{} // Initialize statistics struct
	stats.tcpConnected = false // Initialize TCP connection status

	// Add 4 to wait group for fifoReader, tcpWriter, fileWriter, and statsPrinter
	wg.Add(4)

	// Start the FIFO reader goroutine
	go fifoReader(dataChannelToTCP, dataChannelToFile, &wg, stats, fifoPath)

	// Start the TCP writer goroutine
	go tcpWriter(dataChannelToTCP, &wg, tcpAddr, stats)

	// Start the File writer goroutine
	go fileWriter(dataChannelToFile, &wg, stats, filePath)

	// Start the statistics printer goroutine
	go statsPrinter(stats, &wg)

	fmt.Printf("Program started. Listening on FIFO: %s\n", fifoPath)
	fmt.Printf("Will forward data to TCP address: %s\n", tcpAddr)
	fmt.Printf("Will also save data to local file: %s\n", filePath)
	fmt.Printf("TCP data channel buffer size: %d messages\n", channelBufferSize)
	fmt.Printf("File data channel buffer size: %d messages\n", channelBufferSize)
	fmt.Printf("Use 'mkfifo %s' if it doesn't exist.\n", fifoPath)
	fmt.Printf("To send data: echo 'hello' > %s\n", fifoPath)
	fmt.Printf("To test TCP connection: nc -l %s (in another terminal, replace 8080 with your port)\n", tcpAddr)
	fmt.Printf("To view file content: tail -f %s\n", filePath)

	// Keep the main goroutine alive until goroutines finish (they won't in this design)
	wg.Wait()
	close(dataChannelToTCP)
	close(dataChannelToFile)
	fmt.Println("Program exited.")
}

/*
To run this program:

1.  **Save:** Save the code as `main.go`.
2.  **Build:** `go build -o fifo_to_tcp main.go`
3.  **Create FIFO (if it doesn't exist):** `mkfifo /tmp/video_tunnel_out` (or your custom FIFO path)
    * This is a one-time step. If the FIFO already exists, you don't need to create it again.
4.  **Run TCP Listener (in a separate terminal):**
    * You can use `netcat` (nc) as a simple listener. For example, if you want to listen on port 8080: `nc -l 8080` (or `nc -l -p 8080` on some systems)
    * Alternatively, you can write a simple Go TCP server.
5.  **Run the Go program (in another terminal):**
    * **Using default FIFO and file paths:** `./fifo_to_tcp localhost:8080`
    * **Using custom FIFO path:** `./fifo_to_tcp localhost:8080 /tmp/my_custom_fifo`
    * **Using custom FIFO and file paths:** `./fifo_to_tcp localhost:8080 /tmp/my_custom_fifo /var/log/my_data.log`
    (Replace `localhost:8080` with your desired TCP address and port)

6.  **Send Data to FIFO (in yet another terminal):**
    * `echo "Hello from FIFO!" > /tmp/video_tunnel_out` (or your custom FIFO)
    * `cat somefile.txt > /tmp/video_tunnel_out` (or your custom FIFO)
    * Data will appear both on the TCP listener and in the specified log file.

**How to test TCP disconnection:**

* While the `fifo_to_tcp` program is running and connected to your TCP listener (e.g., `nc -l 8080`), simply stop the listener (e.g., by pressing `Ctrl+C` in its terminal).
* You will see messages in the `fifo_to_tcp` terminal indicating "Connection lost. Retrying...".
* You will also see "TCP channel buffer full, dropping..." messages from the `fifoReader` if data is being sent to the FIFO while the TCP is down, and "TCP disconnected, dropping..." messages from the `tcpWriter` as it drains the channel.
* **Important:** Data will continue to be written to the local file even if the TCP connection is down.
* Restart your TCP listener (e.g., `nc -l 8080`) and the `fifo_to_tcp` program should automatically reconnect, and TCP data forwarding will resume.
*/

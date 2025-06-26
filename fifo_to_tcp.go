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
	defaultFifoPath = "/tmp/video_tunnel_out" // Default path to the Linux FIFO (named pipe)
	retryInterval   = 5 * time.Second         // Interval to wait before retrying TCP connection
	statsPrintInterval = 10 * time.Second      // Interval to print statistics
	channelBufferSize  = 20000                  // Buffer size for the data channel (number of []byte slices)
	maxBytesPerMessage = 4096                  // Max bytes in a single message (fifo read chunk size)
)

// ChannelStats holds the statistics for data flowing through the channel.
type ChannelStats struct {
	mu          sync.Mutex
	bytesSent   uint64
	messagesSent uint64
	bufferedMessages uint64 // Number of messages currently in the channel buffer
	bufferedBytes    uint64 // Total bytes currently in the channel buffer
}

// fifoReader reads data from the specified FIFO and sends it to the dataChannel.
// It also updates the provided ChannelStats.
func fifoReader(dataChannel chan<- []byte, wg *sync.WaitGroup, stats *ChannelStats, fifoPath string) {
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

			// Before sending, update buffered stats
			stats.mu.Lock()
			stats.bufferedMessages++
			stats.bufferedBytes += uint64(n)
			stats.mu.Unlock()

			dataChannel <- data // Send read data to the channel

			// Update total statistics after sending to channel
			stats.mu.Lock()
			stats.bytesSent += uint64(n)
			stats.messagesSent++
			stats.mu.Unlock()
			// fmt.Printf("Read %d bytes from FIFO\n", n) // Debug print
		}
	}
}

// tcpWriter receives data from the dataChannel and sends it over a TCP connection.
// It handles connection retries if the connection is lost.
func tcpWriter(dataChannel <-chan []byte, wg *sync.WaitGroup, tcpAddr string, stats *ChannelStats) {
	defer wg.Done()

	var conn net.Conn
	var err error

	for {
		// Connection loop
		for conn == nil {
			fmt.Printf("Attempting to connect to TCP server: %s\n", tcpAddr)
			conn, err = net.Dial("tcp", tcpAddr)
			if err != nil {
				fmt.Printf("Error connecting to TCP server: %v. Retrying in %v...\n", err, retryInterval)
				time.Sleep(retryInterval)
				continue
			}
			fmt.Printf("Successfully connected to TCP server: %s\n", tcpAddr)
		}

		// Data writing loop
		select {
		case data, ok := <-dataChannel:
			if !ok {
				// Channel closed, exit goroutine
				fmt.Println("Data channel closed, exiting TCP writer.")
				if conn != nil {
					conn.Close()
				}
				return
			}

			// Update buffered stats after receiving from channel
			stats.mu.Lock()
			stats.bufferedMessages--
			stats.bufferedBytes -= uint64(len(data)) // Decrement by the size of the data received
			stats.mu.Unlock()

			_, err := conn.Write(data)
			if err != nil {
				fmt.Printf("Error writing to TCP connection: %v. Connection lost. Retrying...\n", err)
				conn.Close() // Close the broken connection
				conn = nil   // Reset connection to trigger reconnect loop
				// Do not 'return' here, we want to re-enter the connection loop
			} else {
				// fmt.Printf("Sent %d bytes to TCP\n", n) // Debug print
			}
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
		fmt.Printf("Total Bytes Sent (FIFO to Channel): %d\n", stats.bytesSent)
		fmt.Printf("Total Messages Sent (FIFO to Channel): %d\n", stats.messagesSent)
		fmt.Printf("Current Buffered Messages (in Channel): %d\n", stats.bufferedMessages)
		fmt.Printf("Current Buffered Bytes (in Channel): %d\n", stats.bufferedBytes)
		fmt.Printf("--------------------------\n")
		stats.mu.Unlock()
	}
}

func main() {
	if len(os.Args) < 2 || len(os.Args) > 3 {
		fmt.Println("Usage: ./fifo_to_tcp <tcp_address> [fifo_path]")
		fmt.Println("Example: ./fifo_to_tcp localhost:8080")
		fmt.Println("Example: ./fifo_to_tcp localhost:8080 /tmp/my_custom_fifo")
		os.Exit(1)
	}

	tcpAddr := os.Args[1] // Get TCP address from command-line argument
	fifoPath := defaultFifoPath // Initialize with default FIFO path

	if len(os.Args) == 3 {
		fifoPath = os.Args[2] // If provided, use the second argument as FIFO path
	}

	var wg sync.WaitGroup
	// Create a buffered channel
	dataChannel := make(chan []byte, channelBufferSize)
	stats := &ChannelStats{}         // Initialize statistics struct

	// Add 3 to wait group for fifoReader, tcpWriter, and statsPrinter
	wg.Add(3)

	// Start the FIFO reader goroutine, passing the resolved fifoPath
	go fifoReader(dataChannel, &wg, stats, fifoPath)

	// Start the TCP writer goroutine, passing the tcpAddr and stats
	go tcpWriter(dataChannel, &wg, tcpAddr, stats)

	// Start the statistics printer goroutine
	go statsPrinter(stats, &wg)

	fmt.Printf("Program started. Listening on FIFO: %s\n", fifoPath)
	fmt.Printf("Will forward data to TCP address: %s\n", tcpAddr)
	fmt.Printf("Data channel buffer size: %d messages\n", channelBufferSize)
	fmt.Printf("Use 'mkfifo %s' if it doesn't exist.\n", fifoPath)
	fmt.Printf("To send data: echo 'hello' > %s\n", fifoPath)
	fmt.Printf("To test TCP connection: nc -l %s (in another terminal, replace 8080 with your port)\n", tcpAddr)

	// Keep the main goroutine alive until goroutines finish (they won't in this design)
	wg.Wait()
	close(dataChannel) // Close channel when main program intends to exit (not typical for this continuous program)
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
    * **Using default FIFO path:** `./fifo_to_tcp localhost:8080`
    * **Using custom FIFO path:** `./fifo_to_tcp localhost:8080 /tmp/my_custom_fifo`
    (Replace `localhost:8080` with your desired TCP address and port)

6.  **Send Data to FIFO (in yet another terminal):**
    * If using default: `echo "Hello from FIFO!" > /tmp/video_tunnel_out`
    * If using custom: `echo "Hello from FIFO!" > /tmp/my_custom_fifo`
    * `cat somefile.txt > /tmp/video_tunnel_out` (or your custom FIFO)
    * The data sent to the FIFO will be read by the `fifoReader` goroutine and then sent to the `tcpWriter` goroutine, which will forward it to your TCP listener.

**How to test TCP disconnection:**

* While the `fifo_to_tcp` program is running and connected to your TCP listener (e.g., `nc -l 8080`), simply stop the listener (e.g., by pressing `Ctrl+C` in its terminal).
* You will see messages in the `fifo_to_tcp` terminal indicating "Connection lost. Retrying...".
* Restart your TCP listener (e.g., `nc -l 8080`) and the `fifo_to_tcp` program should automatically reconnect.
*/

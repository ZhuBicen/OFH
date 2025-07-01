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
	corruptMessages uint64
}

func fifoReader(dataChannelToTCP chan<- []byte, wg *sync.WaitGroup, stats *ChannelStats, fifoPath string) {
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
				time.Sleep(retryInterval) // Wait before retrying connection
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
		fmt.Printf("  Corrupt Messages: %d\n", stats.corruptMessages)
		fmt.Printf("  File Buffered Messages: %d\n", stats.fileBufferedMessages)
		fmt.Printf("--------------------------\n")
		stats.mu.Unlock()
	}
}

func main() {
	if len(os.Args) < 2 || len(os.Args) > 4 {
		fmt.Println("Usage: ./fifo_to_tcp <tcp_address> [fifo_path]")
		fmt.Println("Example: ./fifo_to_tcp localhost:8080")
		fmt.Println("Example: ./fifo_to_tcp localhost:8080 /tmp/video_tunnel_out")
		os.Exit(1)
	}

	tcpAddr := os.Args[1] 
	fifoPath := defaultFifoPath 

	if len(os.Args) >= 3 {
		fifoPath = os.Args[2] 
	}

	var wg sync.WaitGroup
	dataChannelToTCP := make(chan []byte, channelBufferSize)
	stats := &ChannelStats{} 
	stats.tcpConnected = false

	wg.Add(3)
	go fifoReader(dataChannelToTCP, &wg, stats, fifoPath)
	go tcpWriter(dataChannelToTCP, &wg, tcpAddr, stats)
	go statsPrinter(stats, &wg)

	fmt.Printf("Program started. Listening on FIFO: %s\n", fifoPath)
	fmt.Printf("Will forward data to TCP address: %s\n", tcpAddr)
	fmt.Printf("TCP data channel buffer size: %d messages\n", channelBufferSize)

	wg.Wait()
	close(dataChannelToTCP)
	fmt.Println("Program exited.")
}
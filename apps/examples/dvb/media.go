package main

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"sync"
	"syscall"
	"time"
)

const (
	defaultFifoPathOut      = "/tmp/output_stream"     // Default path to the Linux FIFO (named pipe) - output to TCP
	defaultFifoPathIn    = "/tmp/input_stream"      // Default path for the second input FIFO
	defaultFfmpegOutFifo = "/tmp/ffmpeg_out"           // Default path for ffmpeg output FIFO
	retryInterval        = 5 * time.Second             // Interval to wait before retrying TCP connection
	statsPrintInterval   = 10 * time.Second            // Interval to print statistics
	channelBufferSize    = 300000                       // Buffer size for each data channel (number of []byte slices)
	maxBytesPerMessage   = 4096                        // Max bytes in a single message (fifo read chunk size)
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

// FfmpegRunner holds the state for the ffmpeg process and its control
type FfmpegRunner struct {
	cmd    *exec.Cmd
	done   chan struct{}
	paused bool
	mu     sync.Mutex
}

// startFfmpeg starts the ffmpeg process and forwards data to the input FIFO
func (f *FfmpegRunner) startFfmpeg(inputFile, ffmpegOutFifo, inputFifo string) error {
	// Ensure the input FIFO exists
	if err := ensureFifoExists(inputFifo); err != nil {
		return fmt.Errorf("failed to create input FIFO %s: %w", inputFifo, err)
	}

	// Build ffmpeg command
	f.cmd = exec.Command("ffmpeg", "-y", "-re", "-i", inputFile, "-codec", "copy", "-f", "mpegts", ffmpegOutFifo)
	f.cmd.Stdout = os.Stdout
	f.cmd.Stderr = os.Stderr

	fmt.Printf("Starting ffmpeg: ffmpeg -y -re -i %s -codec copy -f mpegts %s\n", inputFile, ffmpegOutFifo)

	if err := f.cmd.Start(); err != nil {
		return fmt.Errorf("failed to start ffmpeg: %w", err)
	}

	f.done = make(chan struct{})

	// Start a goroutine to handle ffmpeg process completion
	go func() {
		f.cmd.Wait()
		close(f.done)
		fmt.Println("Ffmpeg process finished")
	}()

	return nil
}

// Stop stops reading from the ffmpeg output FIFO
func (f *FfmpegRunner) Stop() {
	f.mu.Lock()
	f.paused = true
	f.mu.Unlock()
	fmt.Println("FfmpegRunner: STOP received, pausing reading from ffmpeg output")
}

// Cont resumes reading from the ffmpeg output FIFO
func (f *FfmpegRunner) Cont() {
	f.mu.Lock()
	f.paused = false
	f.mu.Unlock()
	fmt.Println("FfmpegRunner: CONT received, resuming reading from ffmpeg output")
}

// IsPaused returns whether the runner is paused
func (f *FfmpegRunner) IsPaused() bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.paused
}

// Wait waits for the ffmpeg process to finish
func (f *FfmpegRunner) Wait() {
	<-f.done
}

// ensureFifoExists creates a FIFO if it doesn't exist
func ensureFifoExists(path string) error {
	_, err := os.Stat(path)
	if err == nil {
		// FIFO already exists
		return nil
	}
	if !os.IsNotExist(err) {
		return err
	}
	// FIFO doesn't exist, create it
	return syscall.Mkfifo(path, 0666)
}

// ffmpegToFifo waits for input FIFO to be opened, then starts ffmpeg and forwards data
// It respects STOP/CONT signals to pause/resume
func ffmpegToFifo(ffmpegOutFifo, inputFifo string, wg *sync.WaitGroup, runner *FfmpegRunner, inputFile string) {
	defer wg.Done()

	// First, ensure the input FIFO exists
	if err := ensureFifoExists(inputFifo); err != nil {
		fmt.Printf("ffmpegToFifo: Error creating input FIFO %s: %v\n", inputFifo, err)
		return
	}

	// Wait for input FIFO to be opened by a consumer (non-blocking open with O_WRONLY will fail if no reader)
	for {
		testFile, err := os.OpenFile(inputFifo, os.O_WRONLY|syscall.O_NONBLOCK, 0600)
		if err == nil {
			// Successfully opened - this means a reader is connected
			testFile.Close() // Close the test handle, we'll reopen later
			fmt.Printf("ffmpegToFifo: Input FIFO opened by consumer, starting ffmpeg...\n")
			break
		}
		fmt.Printf("ffmpegToFifo: Waiting for input FIFO %s to be opened by consumer...\n", inputFifo)
		time.Sleep(1000 * time.Millisecond)
	}

	// Now start ffmpeg
	fmt.Printf("ffmpegToFifo: Starting ffmpeg with input: %s\n", inputFile)
	if err := runner.startFfmpeg(inputFile, ffmpegOutFifo, inputFifo); err != nil {
		fmt.Printf("ffmpegToFifo: Error starting ffmpeg: %v\n", err)
		return
	}

	// Open ffmpeg output FIFO for reading
	fmt.Printf("ffmpegToFifo: Opening %s for reading\n", ffmpegOutFifo)
	outFile, err := os.OpenFile(ffmpegOutFifo, os.O_RDONLY, 0600)
	if err != nil {
		fmt.Printf("Error opening ffmpeg output FIFO %s: %v\n", ffmpegOutFifo, err)
		return
	}
	defer outFile.Close()

	// Re-open input FIFO in blocking mode for writing
	fmt.Printf("ffmpegToFifo: Opening %s for writing\n", inputFifo)
	inFile, err := os.OpenFile(inputFifo, os.O_WRONLY, 0600)
	if err != nil {
		fmt.Printf("Error opening input FIFO %s: %v\n", inputFifo, err)
		return
	}
	defer inFile.Close()

	reader := bufio.NewReader(outFile)
	buffer := make([]byte, maxBytesPerMessage)

	fmt.Println("ffmpegToFifo: Started copying data")

	for {
		// Check if paused
		for runner.IsPaused() {
			time.Sleep(100 * time.Millisecond)
		}

		n, err := reader.Read(buffer)
		if err != nil {
			if err == io.EOF {
				fmt.Println("ffmpegToFifo: EOF encountered on ffmpeg output, waiting...")
				time.Sleep(1 * time.Second)
				continue
			}
			fmt.Printf("Error reading from ffmpeg output: %v\n", err)
			return
		}
		if n > 0 {
			_, err := inFile.Write(buffer[:n])
			if err != nil {
				fmt.Printf("Error writing to input FIFO: %v\n", err)
				return
			}
		}
	}
}

func main() {
	if len(os.Args) != 3 {
		fmt.Println("Usage: ./fifo_to_tcp <tcp_address> <ffmpeg_input_file>")
		fmt.Println("Example: ./fifo_to_tcp localhost:8080 /path/to/video.mkv")
		os.Exit(1)
	}

	tcpAddr := os.Args[1]
	ffmpegInputFile := os.Args[2]
	inputFifoPath := defaultFifoPathIn // The FIFO to write to (second FIFO)

	// Always use the default output fifo path from the first argument for TCP forwarding
	outputFifoPath := defaultFifoPathOut

	var wg sync.WaitGroup
	dataChannelToTCP := make(chan []byte, channelBufferSize)
	stats := &ChannelStats{}
	stats.tcpConnected = false

	// Create ffmpeg output FIFO if needed
	if err := ensureFifoExists(defaultFfmpegOutFifo); err != nil {
		fmt.Printf("Error creating ffmpeg output FIFO: %v\n", err)
		os.Exit(1)
	}

	// Create ffmpeg runner (ffmpeg will be started when input FIFO is opened)
	ffmpegRunner := &FfmpegRunner{}
	fmt.Printf("Ffmpeg will start when input FIFO %s is opened by consumer\n", inputFifoPath)

	// Set up signal handling for STOP/CONT
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGUSR1, syscall.SIGUSR2)
	go func() {
		for {
			sig := <-sigChan
			switch sig {
			case syscall.SIGUSR1:
				ffmpegRunner.Stop()
			case syscall.SIGUSR2:
				ffmpegRunner.Cont()
			}
		}
	}()

	wg.Add(4)
	go fifoReader(dataChannelToTCP, &wg, stats, outputFifoPath)
	go tcpWriter(dataChannelToTCP, &wg, tcpAddr, stats)
	go statsPrinter(stats, &wg)

	// Start the ffmpeg to input FIFO copier (will wait for input FIFO to be opened)
	go ffmpegToFifo(defaultFfmpegOutFifo, inputFifoPath, &wg, ffmpegRunner, ffmpegInputFile)

	fmt.Printf("Program started. Listening on FIFO: %s\n", outputFifoPath)
	fmt.Printf("Will forward data to TCP address: %s\n", tcpAddr)
	fmt.Printf("TCP data channel buffer size: %d messages\n", channelBufferSize)
	fmt.Printf("Ffmpeg output FIFO: %s\n", defaultFfmpegOutFifo)
	fmt.Printf("Input FIFO (for ffmpeg data): %s\n", inputFifoPath)
	fmt.Printf("Send SIGUSR1 to pause reading, SIGUSR2 to resume\n")

	wg.Wait()
	close(dataChannelToTCP)

	// Wait for ffmpeg to finish
	ffmpegRunner.Wait()

	fmt.Println("Program exited.")
}
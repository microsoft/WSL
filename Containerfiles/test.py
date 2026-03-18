import asyncio
import websockets
import json
import subprocess
import time
import sys
import requests
from pathlib import Path
from threading import Thread
import base64
import logging

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    force=True
)
logger = logging.getLogger(__name__)

# Color codes
COLOR_RESET = "\033[0m"
COLOR_SERVER = "\033[94m"  # Blue
COLOR_TEST = "\033[96m"    # Cyan
COLOR_SUCCESS = "\033[92m" # Green
COLOR_WARNING = "\033[93m" # Yellow
COLOR_ERROR = "\033[91m"   # Red


def stream_output(pipe, prefix, color):
    """Stream output from subprocess with colored prefix"""
    for line in iter(pipe.readline, b''):
        if line:
            print(f"{color}{prefix}{line.decode().rstrip()}{COLOR_RESET}")
    pipe.close()


def wait_for_server(timeout=60):
    """Wait for the server to be ready"""
    logger.info("Waiting for server to be ready...")
    start_time = time.time()
    
    while time.time() - start_time < timeout:
        try:
            response = requests.get("http://localhost:8000/")
            if response.status_code == 200:
                logger.info("Server is ready!")
                return True
        except requests.exceptions.ConnectionError:
            pass
        time.sleep(1)
        print(f"{COLOR_TEST}.{COLOR_RESET}", end="", flush=True)
    
    logger.error("Timeout waiting for server")
    return False


async def test_websocket():
    # Read the audio file
    audio_path = Path("test.mp3")
    if not audio_path.exists():
        logger.error(f"Error: {audio_path} not found!")
        return
    
    logger.info(f"Loading audio file: {audio_path}")
    with open(audio_path, "rb") as f:
        audio_data = f.read()
    
    logger.info(f"Audio file size: {len(audio_data)} bytes ({len(audio_data) / 1024 / 1024:.2f} MB)")
    
    # Connect to WebSocket
    uri = "ws://localhost:8000/ws"
    logger.info(f"Connecting to {uri}...")
    
    async with websockets.connect(uri) as websocket:
        logger.info("Connected!")
        
        # Send chunked audio data
        CHUNK_SIZE = 64 * 1024  # 64KB chunks
        total_size = len(audio_data)
        chunk_count = (total_size + CHUNK_SIZE - 1) // CHUNK_SIZE  # Ceiling division
        
        logger.info(f"Sending load_start command (total_size={total_size}, chunk_count={chunk_count})...")
        load_start_command = {
            "type": "load_start",
            "total_size": total_size,
            "chunk_count": chunk_count
        }
        await websocket.send(json.dumps(load_start_command))
        
        # Wait for acknowledgment
        ack_message = await websocket.recv()
        ack_event = json.loads(ack_message)
        if ack_event.get("type") == "load_start_ack":
            logger.info("Received load_start_ack, sending chunks...")
        
        # Send audio data in chunks
        for i in range(0, total_size, CHUNK_SIZE):
            chunk = audio_data[i:i + CHUNK_SIZE]
            await websocket.send(chunk)  # Send as binary
            logger.info(f"Sent chunk {i // CHUNK_SIZE + 1}/{chunk_count} ({len(chunk)} bytes)")
        
        logger.info("All chunks sent, sending load_complete command...")
        load_complete_command = {
            "type": "load_complete"
        }
        await websocket.send(json.dumps(load_complete_command))
        
        print(COLOR_SUCCESS + "="*60)
        print("TRANSCRIPTION RESULTS - INITIAL LOAD")
        print("="*60 + COLOR_RESET + "\n")
        
        # Track when to send seek commands
        segments_received = 0
        first_seek_sent = False
        second_seek_sent = False
        
        # Receive and print events
        while True:
            try:
                message = await websocket.recv()
                event = json.loads(message)
                
                if event["type"] == "loaded":
                    print(f"{COLOR_SUCCESS}[EVENT]{COLOR_RESET} Audio loaded successfully")
                    
                elif event["type"] == "processing_started":
                    seek_time = event.get("seek_time", 0)
                    print(f"{COLOR_SUCCESS}[EVENT]{COLOR_RESET} Processing started from {seek_time}s\n")
                    
                elif event["type"] == "processing_cancelled":
                    seek_time = event.get("seek_time", 0)
                    print(f"\n{COLOR_WARNING}[EVENT]{COLOR_RESET} Processing cancelled (was at {seek_time}s)\n")
                    segments_received = 0  # Reset counter
                    
                elif event["type"] == "subtitle":
                    segments_received += 1
                    original_text = event['text']
                    english_text = event.get('text_en', original_text)
                    language = event.get('language', 'unknown')
                    
                    print(f"{COLOR_WARNING}[{event['start']:6.2f}s - {event['end']:6.2f}s]{COLOR_RESET} {original_text}")
                    if language != 'en' and english_text != original_text:
                        print(f"{COLOR_SUCCESS}                    [EN]{COLOR_RESET} {english_text}")
                    
                    # Send first seek after first few segments
                    if segments_received == 20 and not first_seek_sent:
                        logger.info("Sending first SEEK command to 160s...")
                        seek_command = {
                            "type": "seek",
                            "seek_time": 160.0
                        }
                        await websocket.send(json.dumps(seek_command))
                        first_seek_sent = True
                    
                    # Send second seek after 5 segments (of the first seek)
                    elif segments_received == 50 and first_seek_sent and not second_seek_sent:
                        logger.info("Sending second SEEK command to 340s...")
                        seek_command = {
                            "type": "seek",
                            "seek_time": 340.0
                        }
                        await websocket.send(json.dumps(seek_command))
                        second_seek_sent = True
                        
                elif event["type"] == "completed":
                    seek_time = event.get("seek_time", 0)
                    language = event.get("language", "unknown")
                    print(f"\n{COLOR_SUCCESS}" + "="*60)
                    print(f"TRANSCRIPTION COMPLETE (from {seek_time}s, language: {language})")
                    print("="*60 + COLOR_RESET)
                    print(f"\n{COLOR_SUCCESS}Full text ({language}):{COLOR_RESET} {event['full_text']}")
                    
                    full_text_en = event.get('full_text_en')
                    if full_text_en and full_text_en != event['full_text']:
                        print(f"{COLOR_SUCCESS}Full text (en):{COLOR_RESET} {full_text_en}")
                    print()
                    
                    # If we haven't sent both seeks yet, this was the final completion
                    if second_seek_sent:
                        logger.info(f"{COLOR_SUCCESS}All seek tests completed successfully!{COLOR_RESET}")
                        break
                    
                elif event["type"] == "error":
                    logger.error(f"Server error: {event['message']}")
                    break
                    
            except websockets.exceptions.ConnectionClosed:
                logger.warning("Connection closed")
                break


def main():
    test_start_time = time.time()
    
    print(COLOR_SUCCESS + "="*60)
    print("WHISPER WEBSOCKET TRANSCRIPTION TEST")
    print("="*60 + COLOR_RESET + "\n")
    
    # Start the server
    logger.info("Starting server...")
    server_process = subprocess.Popen(
        ["uv", "run", "main.py"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    
    # Start threads to stream server output
    stdout_thread = Thread(
        target=stream_output, 
        args=(server_process.stdout, "[SERVER] ", COLOR_SERVER),
        daemon=True
    )
    stderr_thread = Thread(
        target=stream_output, 
        args=(server_process.stderr, "[SERVER] ", COLOR_SERVER),
        daemon=True
    )
    stdout_thread.start()
    stderr_thread.start()
    
    # Wait for server to be ready
    if not wait_for_server():
        logger.error("Server failed to start")
        server_process.terminate()
        return
    
    print()  # Add blank line for readability
    
    success = False
    try:
        # Run the WebSocket test
        asyncio.run(test_websocket())
        success = True
    except Exception as e:
        logger.exception(f"Test failed with error: {e}")
    finally:
        # Stop the server
        logger.info("Stopping server...")
        server_process.terminate()
        server_process.wait()
        logger.info("Server stopped")
        
        # Calculate and display test duration
        test_duration = time.time() - test_start_time
        
        print("\n" + COLOR_SUCCESS + "="*60)
        if success:
            print(f"✓ TEST COMPLETED SUCCESSFULLY")
        else:
            print(f"{COLOR_ERROR}✗ TEST FAILED{COLOR_SUCCESS}")
        print(f"Total time: {test_duration:.2f} seconds")
        print("="*60 + COLOR_RESET + "\n")


if __name__ == "__main__":
    main()


from fastapi import FastAPI, WebSocket
from fastapi.responses import JSONResponse
from starlette.websockets import WebSocketDisconnect
from contextlib import asynccontextmanager
import uvicorn
from faster_whisper import WhisperModel
import json
import tempfile
import os
import asyncio
from pathlib import Path
import base64
import logging

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    force=True
)
logger = logging.getLogger(__name__)

VERSION = "0.1.0"
model = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup
    global model
    logger.info("Loading Whisper model...")
    # Use CUDA with float16 for GPU acceleration
    model = WhisperModel("base", device="cuda", compute_type="float16")
    logger.info("Whisper model loaded successfully on CUDA GPU")
    yield
    # Shutdown (cleanup if needed)


app = FastAPI(lifespan=lifespan)


@app.get("/")
async def root():
    return JSONResponse({"version": VERSION})


def find_overlapping_translation(orig_start: float, orig_end: float, translation_segments: list) -> str:
    """
    Find translation segments that overlap with the original segment's time range.
    Combines text from all overlapping segments, weighted by overlap amount.
    """
    overlapping_texts = []
    
    for trans_seg in translation_segments:
        # Calculate overlap
        overlap_start = max(orig_start, trans_seg.start)
        overlap_end = min(orig_end, trans_seg.end)
        overlap_duration = overlap_end - overlap_start
        
        # If there's meaningful overlap (more than 10% of original segment)
        orig_duration = orig_end - orig_start
        if overlap_duration > 0 and overlap_duration / orig_duration > 0.1:
            overlapping_texts.append(trans_seg.text.strip())
    
    # Combine all overlapping translations
    if overlapping_texts:
        return " ".join(overlapping_texts)
    
    # Fallback: find the closest segment by midpoint
    orig_mid = (orig_start + orig_end) / 2
    closest_seg = min(translation_segments, 
                     key=lambda seg: abs((seg.start + seg.end) / 2 - orig_mid))
    return closest_seg.text.strip()


async def transcribe_from_position(websocket: WebSocket, audio_path: str, seek_time: float, cancel_event: asyncio.Event):
    """
    Transcribe audio from a specific position, cancellable via cancel_event.
    Detects language and provides both original and English translation, streaming results.
    Processes both streams simultaneously and sends events as soon as matches are ready.
    """
    temp_sliced_audio = None
    try:
        # Send processing started event
        await websocket.send_text(json.dumps({
            "type": "processing_started",
            "seek_time": seek_time
        }))
        
        # If seeking, create a sliced version of the audio starting from seek_time
        if seek_time > 0:

            logger.info(f"Slicing audio from {seek_time}s for transcription...")
            with tempfile.NamedTemporaryFile(delete=False, suffix=".wav") as temp_sliced:
                temp_sliced_audio = temp_sliced.name
            
            # Use ffmpeg to slice the audio from seek_time onwards
            import subprocess
            result = await asyncio.to_thread(
                subprocess.run,
                ["ffmpeg", "-i", audio_path, "-ss", str(seek_time), "-c", "copy", temp_sliced_audio, "-y"],
                capture_output=True,
                text=True
            )
            logger.info(f"ffmpeg output: {result.stdout}")
            
            if result.returncode != 0:
                logger.error(f"ffmpeg error: {result.stderr}")
                raise Exception(f"Failed to slice audio: {result.stderr}")
            
            # Use the sliced audio for transcription
            transcribe_audio_path = temp_sliced_audio
        else:
            # No seeking needed, use original audio
            transcribe_audio_path = audio_path
        
        # First, do a quick language detection (always use original audio for this)
        logger.info("Detecting language...")
        
        def detect_language():
            segments_iter, info = model.transcribe(audio_path, task="transcribe")
            return info.language, info.language_probability
        
        detected_language, language_probability = await asyncio.to_thread(detect_language)
        logger.info(f"Detected language: {detected_language} (probability: {language_probability:.2f})")
        
        # Now start both transcription and translation streams
        original_queue = asyncio.Queue()
        translation_queue = asyncio.Queue()
        
        # Capture the event loop
        loop = asyncio.get_event_loop()
        
        def produce_segments(task_type: str, queue: asyncio.Queue, loop):
            """Run transcription in thread and put segments into queue"""
            try:
                segments_iter, _ = model.transcribe(transcribe_audio_path, task=task_type)
                for segment in segments_iter:
                    # Adjust timestamps by adding seek_time back
                    segment.start += seek_time
                    segment.end += seek_time
                    asyncio.run_coroutine_threadsafe(queue.put(segment), loop).result()
            except Exception as e:
                logger.error(f"Error in {task_type}: {e}")
            finally:
                asyncio.run_coroutine_threadsafe(queue.put(None), loop).result()
        
        # Start both transcriptions in parallel threads
        import concurrent.futures
        executor = concurrent.futures.ThreadPoolExecutor(max_workers=2)
        executor.submit(produce_segments, "transcribe", original_queue, loop)
        
        if detected_language != "en":
            executor.submit(produce_segments, "translate", translation_queue, loop)
        
        # Track segments for matching
        translation_buffer = []  # Buffer of all translation segments seen so far
        full_text_parts = []
        full_text_en_parts = []
        segment_count = 0
        
        # Process original segments as they arrive
        while True:
            if cancel_event.is_set():
                await websocket.send_text(json.dumps({
                    "type": "processing_cancelled",
                    "seek_time": seek_time
                }))
                return
            
            # Get next original segment (with timeout to check cancellation)
            try:
                orig_segment = await asyncio.wait_for(original_queue.get(), timeout=0.5)
            except asyncio.TimeoutError:
                continue
            
            if orig_segment is None:  # Stream ended
                break
            
            # For non-English, wait until we have translation coverage
            if detected_language != "en":
                # Collect translation segments until we cover this original segment
                while True:
                    # Check if we already have coverage
                    if translation_buffer and translation_buffer[-1].end >= orig_segment.end:
                        break
                    
                    # Get more translation segments
                    try:
                        trans_segment = await asyncio.wait_for(translation_queue.get(), timeout=0.1)
                        if trans_segment is None:  # Translation stream ended
                            break
                        translation_buffer.append(trans_segment)
                    except asyncio.TimeoutError:
                        # Check if we have enough coverage anyway
                        if translation_buffer and translation_buffer[-1].end >= orig_segment.end:
                            break
                        continue
            
            # Now we can send this segment
            segment_count += 1
            
            # Find matching translation by time overlap
            if detected_language != "en" and translation_buffer:
                text_en = find_overlapping_translation(
                    orig_segment.start, 
                    orig_segment.end, 
                    translation_buffer
                )
            else:
                text_en = orig_segment.text.strip()
            
            subtitle_event = {
                "type": "subtitle",
                "start": orig_segment.start,
                "end": orig_segment.end,
                "text": orig_segment.text.strip(),
                "text_en": text_en,
                "language": detected_language
            }
            logger.info(f"Sending subtitle segment {segment_count}: {subtitle_event['start']:.2f}s - {subtitle_event['end']:.2f}s")
            await websocket.send_text(json.dumps(subtitle_event))
            full_text_parts.append(orig_segment.text)
            full_text_en_parts.append(text_en)
            
            # Yield control to event loop
            await asyncio.sleep(0)
        
        # Clean up executor
        executor.shutdown(wait=False)
        
        # Only send completion if not cancelled
        if not cancel_event.is_set():
            logger.info(f"Processed {segment_count} segments (seek_time: {seek_time}s, language: {detected_language})")
            completion_event = {
                "type": "completed",
                "full_text": " ".join(full_text_parts).strip(),
                "full_text_en": " ".join(full_text_en_parts).strip(),
                "language": detected_language,
                "seek_time": seek_time
            }
            await websocket.send_text(json.dumps(completion_event))
    except Exception as e:
        logger.exception(f"ERROR in transcribe_from_position: {e}")
        if not cancel_event.is_set():
            try:
                await websocket.send_text(json.dumps({
                    "type": "error",
                    "message": str(e)
                }))
            except Exception as send_error:
                logger.error(f"Failed to send error message: {send_error}")
    finally:
        # Clean up temporary sliced audio file if created
        if temp_sliced_audio and os.path.exists(temp_sliced_audio):
            os.unlink(temp_sliced_audio)


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    temp_audio_path = None
    transcription_task = None
    cancel_event = asyncio.Event()
    
    # State for chunked audio loading
    audio_chunks = []
    expected_total_size = 0
    expected_chunk_count = 0
    receiving_chunks = False
    
    try:
        while True:
            # Receive either text or binary message
            message = await websocket.receive()
            
            # Handle binary chunks
            if "bytes" in message:
                binary_data = message["bytes"]
                logger.info(f"Received binary chunk of {len(binary_data)} bytes")
                
                if not receiving_chunks:
                    logger.warning("Received binary data but not in chunk receiving mode")
                    continue
                
                audio_chunks.append(binary_data)
                logger.info(f"Accumulated {len(audio_chunks)} chunks, total size: {sum(len(c) for c in audio_chunks)} bytes")
                continue
            
            # Handle text commands
            message_text = message.get("text", "")
            logger.info(f"Received websocket message: {message_text[:200]}...")  # Log first 200 chars
            
            try:
                command = json.loads(message_text)
            except json.JSONDecodeError as e:
                logger.error(f"Failed to parse JSON message: {e}")
                await websocket.send_text(json.dumps({
                    "type": "error",
                    "message": f"Invalid JSON: {str(e)}"
                }))
                continue
            
            command_type = command.get("type")
            logger.info(f"Processing command type: {command_type}")
            
            if command_type == "load_start":
                # Start receiving chunked audio data
                logger.info("Processing 'load_start' command...")
                expected_total_size = command.get("total_size", 0)
                expected_chunk_count = command.get("chunk_count", 0)
                
                logger.info(f"Expecting {expected_chunk_count} chunks, total size: {expected_total_size} bytes ({expected_total_size / 1024 / 1024:.2f} MB)")
                
                # Cancel any existing transcription
                if transcription_task and not transcription_task.done():
                    logger.info("Cancelling existing transcription task...")
                    cancel_event.set()
                    await transcription_task
                    logger.info("Previous transcription cancelled")
                
                # Clean up old temp file if it exists
                if temp_audio_path and os.path.exists(temp_audio_path):
                    logger.info(f"Cleaning up old temp file: {temp_audio_path}")
                    os.unlink(temp_audio_path)
                
                # Reset chunk buffer
                audio_chunks = []
                receiving_chunks = True
                
                await websocket.send_text(json.dumps({
                    "type": "load_start_ack",
                    "message": "Ready to receive chunks"
                }))
                logger.info("Sent load_start_ack - ready to receive binary chunks")
            
            elif command_type == "load_complete":
                # All chunks received, now process the audio
                logger.info("Processing 'load_complete' command...")
                
                if not receiving_chunks:
                    logger.error("Received load_complete but was not receiving chunks")
                    await websocket.send_text(json.dumps({
                        "type": "error",
                        "message": "Invalid state: not receiving chunks"
                    }))
                    continue
                
                receiving_chunks = False
                
                # Combine all chunks
                logger.info(f"Combining {len(audio_chunks)} chunks...")
                audio_data = b"".join(audio_chunks)
                actual_size = len(audio_data)
                logger.info(f"Total audio data received: {actual_size} bytes ({actual_size / 1024 / 1024:.2f} MB)")
                
                # Verify size matches
                if expected_total_size > 0 and actual_size != expected_total_size:
                    logger.warning(f"Size mismatch! Expected {expected_total_size}, got {actual_size}")
                else:
                    logger.info(f"✓ Size verification passed")
                
                # Save to temp file and convert to WAV
                try:
                    # First, save raw data to a temp file
                    with tempfile.NamedTemporaryFile(delete=False, suffix=".raw") as temp_raw:
                        temp_raw.write(audio_data)
                        temp_raw_path = temp_raw.name
                    
                    logger.info(f"Raw audio saved to: {temp_raw_path}")
                    
                    # Convert raw PCM to WAV using ffmpeg
                    # Assuming 16kHz, 16-bit PCM mono (matching the QT audio decoder settings)
                    with tempfile.NamedTemporaryFile(delete=False, suffix=".wav") as temp_wav:
                        temp_audio_path = temp_wav.name
                    
                    logger.info(f"Converting raw PCM to WAV format...")
                    import subprocess
                    result = await asyncio.to_thread(
                        subprocess.run,
                        [
                            "ffmpeg", "-f", "s16le",  # Input format: signed 16-bit little-endian PCM
                            "-ar", "16000",            # Sample rate: 16kHz
                            "-ac", "1",                # Channels: 1 (mono)
                            "-i", temp_raw_path,       # Input file
                            "-y",                      # Overwrite output
                            temp_audio_path            # Output WAV file
                        ],
                        capture_output=True,
                        text=True
                    )
                    
                    # Clean up raw file
                    os.unlink(temp_raw_path)
                    
                    if result.returncode != 0:
                        logger.error(f"ffmpeg conversion error: {result.stderr}")
                        raise Exception(f"Failed to convert audio: {result.stderr}")
                    
                    logger.info(f"✓ Audio converted to WAV: {temp_audio_path}")
                    
                    import av
                    with av.open(temp_audio_path, mode="r") as container:
                        # Try to get basic info to verify file is valid
                        if container.streams.audio:
                            logger.info(f"✓ WAV file validated - duration: {container.duration / 1000000:.2f}s")
                        else:
                            raise Exception("No audio stream found in file")
                    
                    # Clear chunks from memory
                    audio_chunks = []
                    
                    await websocket.send_text(json.dumps({
                        "type": "loaded",
                        "message": "Audio loaded successfully",
                        "temp_path": temp_audio_path,
                        "size": actual_size
                    }))
                    logger.info("Sent 'loaded' confirmation to client")
                    
                    # Start transcription from beginning
                    logger.info("Starting transcription from beginning...")
                    cancel_event = asyncio.Event()
                    transcription_task = asyncio.create_task(
                        transcribe_from_position(websocket, temp_audio_path, 0.0, cancel_event)
                    )
                    logger.info("Transcription task created and started")
                    
                except Exception as e:
                    logger.exception(f"Error saving audio file: {e}")
                    await websocket.send_text(json.dumps({
                        "type": "error",
                        "message": f"Failed to save audio: {str(e)}"
                    }))
            
            elif command_type == "seek":
                # Seek to a specific position
                logger.info("Processing 'seek' command...")
                seek_time = command.get("seek_time", 0.0)
                logger.info(f"Seek time: {seek_time}s")
                
                if not temp_audio_path:
                    logger.error("Seek command received but no audio is loaded")
                    await websocket.send_text(json.dumps({
                        "type": "error",
                        "message": "No audio loaded. Use 'load' command first."
                    }))
                    continue
                
                # Cancel current transcription
                if transcription_task and not transcription_task.done():
                    logger.info("Cancelling current transcription for seek...")
                    cancel_event.set()
                    await transcription_task
                
                # Start new transcription from seek position
                logger.info(f"Starting transcription from position {seek_time}s")
                cancel_event = asyncio.Event()
                transcription_task = asyncio.create_task(
                    transcribe_from_position(websocket, temp_audio_path, seek_time, cancel_event)
                )
                logger.info("Seek transcription task created")
            
            else:
                logger.warning(f"Received unknown command type: {command_type}")
                await websocket.send_text(json.dumps({
                    "type": "error",
                    "message": f"Unknown command type: {command_type}"
                }))
            
    except WebSocketDisconnect:
        # Client disconnected - just clean up
        logger.info("WebSocket client disconnected")
    except Exception as e:
        # Try to send error if connection is still open
        logger.exception(f"Unexpected error in websocket endpoint: {e}")
        try:
            await websocket.send_text(json.dumps({
                "type": "error",
                "message": str(e)
            }))
        except (WebSocketDisconnect, RuntimeError):
            logger.warning("Could not send error to client - connection closed")
    finally:
        # Cancel any running transcription
        if transcription_task and not transcription_task.done():
            cancel_event.set()
            try:
                await transcription_task
            except:
                pass
        
        # Clean up temporary file
        if temp_audio_path and os.path.exists(temp_audio_path):
            os.unlink(temp_audio_path)
        
        # Try to close the websocket if it's still open
        try:
            await websocket.close()
        except (WebSocketDisconnect, RuntimeError):
            pass


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)

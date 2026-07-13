import requests
import json
import sys
import time

def verify():
    port = 45020
    base_url = f"http://127.0.0.1:{port}"
    session = requests.Session()
    
    # Login
    print("Logging in...")
    login_res = session.post(f"{base_url}/login", json={"username": "admin", "password": "admin123"})
    if login_res.status_code != 200:
        print("Failed to login", login_res.text)
        sys.exit(1)
        
    # Get active streams
    print("Fetching active streams...")
    res = session.get(f"{base_url}/api/streams")
    if res.status_code != 200:
        print("Failed to get streams", res.text)
        sys.exit(1)
    
    streams = res.json()
    if not streams:
        print("No streams found on local server, creating one to test...")
        # Create a mock stream to test
        # First get inputs
        inputs_res = session.get(f"{base_url}/api/inputs")
        inputs = inputs_res.json()
        if not inputs:
            print("No inputs available to create stream.")
            sys.exit(0)
            
        inp = inputs[0]
        create_payload = {
            "name": "TEST PRESET CHANNEL",
            "input_id": inp["id"],
            "program_number": 1,
            "enabled": True,
            "outputs": [{"url": "udp://127.0.0.1:9099", "output_interface": "", "type": "udp"}],
            "transcode_enabled": True,
            "transcode_video": True,
            "video_input_format": "auto",
            "video_output_format": "h264",
            "transcode_audio": False,
            "audio_input_format": "auto",
            "audio_output_format": "aac",
            "limit_bitrate": 4000,
            "transcode_preset": "medium"
        }
        create_res = session.post(f"{base_url}/api/streams", json=create_payload)
        if create_res.status_code != 200:
            print("Failed to create stream", create_res.text)
            sys.exit(1)
            
        stream_id = create_res.json()["id"]
        print(f"Stream created successfully with ID: {stream_id}")
    else:
        # Use existing stream
        stream = streams[0]
        stream_id = stream["id"]
        # Update stream
        print(f"Updating stream {stream_id} with transcode_preset='medium'...")
        update_payload = stream.copy()
        update_payload["transcode_enabled"] = True
        update_payload["transcode_video"] = True
        update_payload["transcode_preset"] = "medium"
        update_res = session.put(f"{base_url}/api/streams/{stream_id}", json=update_payload)
        if update_res.status_code != 200:
            print("Failed to update stream", update_res.text)
            sys.exit(1)
            
    # Verify persistence
    print("Verifying if preset persisted...")
    res = session.get(f"{base_url}/api/streams")
    streams = res.json()
    test_stream = next((s for s in streams if s["id"] == stream_id), None)
    if not test_stream:
        print("Stream not found in streams list")
        sys.exit(1)
        
    preset = test_stream.get("transcode_preset")
    print(f"Verified preset in API response: '{preset}'")
    if preset == "medium":
        print("SUCCESS: Channel-level preset successfully set and verified!")
    else:
        print(f"FAILED: Preset did not persist. Found '{preset}' instead.")
        sys.exit(1)
        
    print("VERIFICATION COMPLETED SUCCESSFULLY!")

if __name__ == "__main__":
    verify()

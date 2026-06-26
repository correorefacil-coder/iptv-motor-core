import subprocess
import time
import urllib.request
import urllib.error
import json
import os
import shutil

PORT = 45020
BASE_URL = f"http://127.0.0.1:{PORT}"

def api_call(path, data=None, method="GET", headers=None):
    url = f"{BASE_URL}{path}"
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)
    
    req_data = json.dumps(data).encode('utf-8') if data else None
    req = urllib.request.Request(url, data=req_data, headers=req_headers, method=method)
    try:
        with urllib.request.urlopen(req) as res:
            res_body = res.read().decode('utf-8')
            if not res_body:
                return {"success": True}
            try:
                return json.loads(res_body)
            except json.JSONDecodeError:
                return res_body
    except urllib.error.HTTPError as e:
        err_body = e.read().decode('utf-8')
        print(f"HTTPError to {url}: code={e.code}, body={err_body}")
        try:
            return {"status": e.code, "error_info": json.loads(err_body)}
        except json.JSONDecodeError:
            return {"status": e.code, "error_info": err_body}
    except Exception as e:
        print(f"Connection/API Call failed to {url}: {e}")
        return None

def login(username, password):
    data = {"username": username, "password": password}
    url = f"{BASE_URL}/login"
    req_data = json.dumps(data).encode('utf-8')
    req = urllib.request.Request(url, data=req_data, headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req) as res:
            cookies = res.getheader('Set-Cookie')
            res_body = res.read().decode('utf-8')
            body = json.loads(res_body) if res_body else {}
            print(f"Login success for {username}:", body)
            return cookies
    except Exception as e:
        print(f"Login failed for {username}:", e)
        return None

def main():
    config_path = "/home/cristian/Antigravity/config.json"
    backup_path = "/home/cristian/Antigravity/config.json.bak"
    
    # Clean up test directories if any
    test_hls_dir = "/home/cristian/Antigravity/www/hls/testchannel"
    if os.path.exists(test_hls_dir):
        shutil.rmtree(test_hls_dir)

    # Backup config.json
    print("Backing up config.json...")
    if os.path.exists(config_path):
        shutil.copyfile(config_path, backup_path)
    
    # Start the server process
    print("Starting softproductiva_iptv server...")
    server_proc = subprocess.Popen(
        ["/home/cristian/Antigravity/build/softproductiva_iptv", str(PORT), "config.json"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd="/home/cristian/Antigravity"
    )
    
    # Wait for server to start
    time.sleep(2)
    
    try:
        # 1. Login as admin
        cookie = login("admin", "admin123")
        if not cookie:
            print("Could not login as admin.")
            return
        
        headers = {"Cookie": cookie}
        
        # 2. Get inputs to find a valid video pack input
        inputs = api_call("/api/inputs", headers=headers)
        if not inputs or len(inputs) == 0:
            print("No inputs available to create a stream. Inputs response:", inputs)
            return
        
        target_input = None
        for inp in inputs:
            if inp.get("is_video_pack"):
                target_input = inp
                break
                
        if not target_input:
            print("No video pack input found.")
            return
            
        target_input_id = target_input["id"]
        print(f"Using video pack input source {target_input_id} ({target_input['name']})")
        
        # Ensure the input is enabled
        print(f"Enabling input {target_input_id}...")
        enable_payload = {
            "name": target_input["name"],
            "url": target_input["url"],
            "is_video_pack": True,
            "enabled": True
        }
        enable_res = api_call(f"/api/inputs/{target_input_id}", data=enable_payload, method="PUT", headers=headers)
        print("Input enable response:", enable_res)

        # 3. Create a stream with multiple outputs: one UDP, one RTP, one HLS
        hls_path = "www/hls/testchannel/index.m3u8"
        outputs = [
            {"url": "udp://239.1.1.250:5000", "output_interface": "", "type": "udp"},
            {"url": "rtp://239.1.1.251:5002", "output_interface": "", "type": "rtp"},
            {"url": hls_path, "output_interface": "", "type": "hls"}
        ]
        
        payload = {
            "name": "Test Multiple Outputs",
            "input_id": target_input_id,
            "program_number": 1,
            "enabled": True,
            "outputs": outputs,
            "video_filename": "video2.avi"
        }
        
        print("Creating stream with multiple outputs...")
        create_res = api_call("/api/streams", data=payload, method="POST", headers=headers)
        print("Create response:", create_res)
        assert create_res is not None
        assert create_res.get("id") is not None
        stream_id = create_res["id"]
        
        # Wait a bit for FFmpeg to write segments
        print("Waiting for HLS directory and playlist generation...")
        playlist_path = f"/home/cristian/Antigravity/{hls_path}"
        
        # Try checking for playlist for 15 seconds
        playlist_exists = False
        for i in range(15):
            if os.path.exists(playlist_path):
                playlist_exists = True
                print(f"HLS playlist index.m3u8 found at {playlist_path}!")
                break
            time.sleep(1)
            
        assert playlist_exists, "HLS playlist was not created!"
        
        # 4. Fetch the HLS playlist via HTTP server
        hls_http_url = f"/hls/testchannel/index.m3u8"
        print(f"Fetching HLS playlist from HTTP server: {hls_http_url}")
        
        # Read raw URL for HLS
        req = urllib.request.Request(f"{BASE_URL}{hls_http_url}", headers=headers)
        with urllib.request.urlopen(req) as res:
            raw_playlist = res.read().decode('utf-8')
            print("Fetched playlist content successfully:")
            print(raw_playlist)
            assert "#EXTM3U" in raw_playlist, "Invalid HLS playlist content"
            
        # 5. Verify config.json parsed values and serialization
        print("Verifying config.json contents...")
        with open(config_path, "r") as f:
            config_data = json.load(f)
            
        target_stream = None
        for s in config_data.get("streams", []):
            if s.get("id") == stream_id:
                target_stream = s
                break
                
        assert target_stream is not None, "Created stream not found in config.json"
        print("Stream configuration in config.json:")
        print(json.dumps(target_stream, indent=2))
        
        # Assertions
        assert "outputs" in target_stream, "outputs array missing in config.json stream"
        assert len(target_stream["outputs"]) == 3, "Wrong outputs count in config.json"
        assert target_stream["outputs"][0]["type"] == "udp"
        assert target_stream["outputs"][1]["type"] == "rtp"
        assert target_stream["outputs"][2]["type"] == "hls"
        
        # Check backward compatibility fields are saved
        assert target_stream["output_url"] == "udp://239.1.1.250:5000"
        
        # 6. Test Programmer role restrictions (cannot modify outputs)
        prog_cookie = login("comunicaciones", "comunicaciones")
        if not prog_cookie:
            print("Could not login as communications programmer.")
            return
            
        prog_headers = {"Cookie": prog_cookie}
        
        # Modify the stream as a programmer on an unauthorized stream
        payload_prog = payload.copy()
        payload_prog["outputs"] = [
            {"url": "udp://239.1.1.250:5000", "output_interface": "", "type": "udp"}
        ]
        
        print("Attempting to modify outputs list as programmer on unauthorized stream (should fail)...")
        update_res = api_call(f"/api/streams/{stream_id}", data=payload_prog, method="PUT", headers=prog_headers)
        print("Programmer unauthorized update response:", update_res)
        assert update_res.get("status") == 403, f"Expected 403 Forbidden, got {update_res.get('status')}"
        assert "No tiene permisos para modificar este canal." in update_res.get("error_info", {}).get("error", ""), "Wrong error message for programmer unauthorized stream"
        
        # Modify the stream as a programmer on an authorized stream
        allowed_stream_id = "stream_inst_1"
        allowed_stream = None
        for s in config_data.get("streams", []):
            if s.get("id") == allowed_stream_id:
                allowed_stream = s
                break
                
        assert allowed_stream is not None, "stream_inst_1 not found in config"
        payload_allowed = allowed_stream.copy()
        payload_allowed["outputs"] = [
            {"url": "udp://239.9.9.9:9999", "output_interface": "", "type": "udp"}
        ]
        
        print("Attempting to modify outputs list as programmer on authorized stream (should fail)...")
        update_res_2 = api_call(f"/api/streams/{allowed_stream_id}", data=payload_allowed, method="PUT", headers=prog_headers)
        print("Programmer authorized update response:", update_res_2)
        assert update_res_2.get("status") == 403, f"Expected 403 Forbidden, got {update_res_2.get('status')}"
        assert "Los programadores solo pueden cambiar el archivo de video" in update_res_2.get("error_info", {}).get("error", ""), "Wrong error message for programmer update restriction"
        print("Programmer restriction tests passed!")

        print("Multiple Outputs Verification SUCCESSFUL!")
        
    finally:
        # Terminate server
        print("Stopping server...")
        server_proc.terminate()
        server_proc.wait()
        
        # Restore config.json
        print("Restoring config.json backup...")
        if os.path.exists(backup_path):
            shutil.copyfile(backup_path, config_path)
            os.remove(backup_path)

if __name__ == "__main__":
    main()

import subprocess
import time
import urllib.request
import json
import socket
import hashlib
import base64
import struct
import os

def make_handshake_key():
    return base64.b64encode(hashlib.sha1(b"test_key").digest()).decode('utf-8')[:16]

def encode_frame(payload):
    payload_bytes = payload.encode('utf-8')
    payload_len = len(payload_bytes)
    
    frame = bytearray([0x81])
    if payload_len <= 125:
        frame.append(0x80 | payload_len)
    elif payload_len <= 65535:
        frame.append(0x80 | 126)
        frame.extend(struct.pack('>H', payload_len))
    else:
        frame.append(0x80 | 127)
        frame.extend(struct.pack('>Q', payload_len))
        
    mask = b'\x12\x34\x56\x78'
    frame.extend(mask)
    
    masked_payload = bytearray(b ^ mask[i % 4] for i, b in enumerate(payload_bytes))
    frame.extend(masked_payload)
    return bytes(frame)

def decode_frame(sock):
    header = sock.recv(2)
    if not header or len(header) < 2:
        return None
    
    fin_opcode = header[0]
    mask_len = header[1]
    
    has_mask = (mask_len & 0x80) != 0
    length = mask_len & 0x7f
    
    if length == 126:
        len_bytes = sock.recv(2)
        length = struct.unpack('>H', len_bytes)[0]
    elif length == 127:
        len_bytes = sock.recv(8)
        length = struct.unpack('>Q', len_bytes)[0]
        
    if has_mask:
        mask = sock.recv(4)
        
    payload = bytearray()
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            break
        payload.extend(chunk)
        
    if has_mask:
        payload = bytearray(b ^ mask[i % 4] for i, b in enumerate(payload))
        
    if (fin_opcode & 0x0f) == 1:
        return payload.decode('utf-8', errors='ignore')
    return payload

def send_command(sock, cmd_id, method, params=None):
    cmd = {"id": cmd_id, "method": method}
    if params:
        cmd["params"] = params
    sock.sendall(encode_frame(json.dumps(cmd)))

def wait_for_response(sock, cmd_id, timeout=10.0):
    sock.settimeout(timeout)
    start_time = time.time()
    while time.time() - start_time < timeout:
        try:
            msg = decode_frame(sock)
            if not msg:
                continue
            data = json.loads(msg)
            
            # Log console and exceptions
            if data.get("method") == "Console.messageAdded":
                msg_text = data["params"]["message"]["text"]
                print(f"[CONSOLE]: {msg_text}")
            elif data.get("method") == "Runtime.exceptionThrown":
                details = data["params"]["exceptionDetails"]
                text = details.get("exception", {}).get("description", details.get("text"))
                print(f"[EXCEPTION]: {text}")
                
            if data.get("id") == cmd_id:
                return data.get("result")
        except socket.timeout:
            break
    raise TimeoutError(f"Command {cmd_id} timed out.")

def main():
    print("Starting Chrome in headless mode...")
    chrome_proc = subprocess.Popen([
        "google-chrome",
        "--headless=new",
        "--disable-gpu",
        "--remote-debugging-port=9222",
        "--window-size=1280,1024",
        "--user-data-dir=/home/cristian/Antigravity/chrome_profile",
        "http://localhost:8000/login.html?autologin=1"
    ])
    
    time.sleep(5)
    
    try:
        req = urllib.request.urlopen("http://localhost:9222/json/list")
        tabs = json.loads(req.read().decode('utf-8'))
        
        target_tab = None
        for tab in tabs:
            if "localhost:8000" in tab.get("url", ""):
                target_tab = tab
                break
                
        if not target_tab:
            print("Target tab not found. Tabs:")
            print(json.dumps(tabs, indent=2))
            return
            
        ws_url = target_tab["webSocketDebuggerUrl"]
        print(f"Connecting to ws: {ws_url}")
        
        parts = ws_url.replace("ws://", "").split("/", 1)
        host_port = parts[0]
        path = "/" + parts[1]
        host, port = host_port.split(":")
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, int(port)))
        
        # Handshake
        key = make_handshake_key()
        handshake = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host_port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        sock.sendall(handshake.encode('utf-8'))
        
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = sock.recv(1024)
            if not chunk:
                break
            response += chunk
            
        print("Connected. Enabling domains...")
        send_command(sock, 1, "Runtime.enable")
        wait_for_response(sock, 1)
        send_command(sock, 2, "Page.enable")
        wait_for_response(sock, 2)
        send_command(sock, 3, "Console.enable")
        wait_for_response(sock, 3)
        
        # Poll URL until we are on index.html
        print("Waiting for page redirection to index.html...")
        on_dashboard = False
        for i in range(10):
            send_command(sock, 100 + i, "Runtime.evaluate", {"expression": "window.location.href"})
            res = wait_for_response(sock, 100 + i)
            url = res["result"].get("value", "")
            print(f"Current URL: {url}")
            if "index.html" in url or url == "http://localhost:8000/" or url == "http://127.0.0.1:8000/":
                on_dashboard = True
                break
            time.sleep(1)
            
        if not on_dashboard:
            print("Failed to redirect to index.html dashboard.")
            return
            
        # Wait until inputs array is loaded and has items
        print("Waiting for inputs to load...")
        inputs_loaded = False
        for i in range(10):
            check_inputs_expr = "typeof inputs !== 'undefined' && inputs !== null && inputs.length > 0"
            send_command(sock, 200 + i, "Runtime.evaluate", {"expression": check_inputs_expr})
            res = wait_for_response(sock, 200 + i)
            if res["result"].get("value") == True:
                inputs_loaded = True
                break
            time.sleep(1)
            
        if not inputs_loaded:
            print("Inputs not loaded in the UI.")
            return
            
        # Get inputs
        send_command(sock, 300, "Runtime.evaluate", {"expression": "JSON.stringify(inputs)", "returnByValue": True})
        res_inputs = wait_for_response(sock, 300)
        inputs_list = json.loads(res_inputs["result"]["value"])
        print(f"Loaded {len(inputs_list)} inputs.")
        target_input_id = inputs_list[0]["id"]
        print(f"Target input to test: {target_input_id}")
        
        # Disable input
        script_disable = f"toggleInput('{target_input_id}', false)"
        print(f"Sending: {script_disable}")
        send_command(sock, 400, "Runtime.evaluate", {"expression": script_disable, "awaitPromise": True})
        wait_for_response(sock, 400)
        
        time.sleep(2)
        
        # Verify config.json
        print("Reading config.json to verify settings...")
        with open("/home/cristian/Antigravity/config.json", "r") as f:
            config = json.load(f)
            
        target_input = None
        for inp in config.get("inputs", []):
            if inp.get("id") == target_input_id:
                target_input = inp
                break
                
        if target_input:
            print(f"Config for {target_input_id} after disable:")
            print(json.dumps(target_input, indent=2))
            assert target_input.get("enabled") == False
            print("SUCCESS: config.json correctly updated with enabled = False.")
        else:
            print(f"ERROR: {target_input_id} not found in config.json")
            return
            
        # Capture screenshot while disabled
        scroll_script = f"document.getElementById('input-{target_input_id}').scrollIntoView({{ block: 'center' }});"
        send_command(sock, 499, "Runtime.evaluate", {"expression": scroll_script})
        wait_for_response(sock, 499)
        time.sleep(1)
        
        send_command(sock, 500, "Page.captureScreenshot", {"format": "png"})
        res_screenshot = wait_for_response(sock, 500)
        screenshot_data = res_screenshot.get("data")
        
        if screenshot_data:
            img_path = "/home/cristian/.gemini/antigravity/brain/cfd0769a-95e9-4627-aeed-6657cd5caf7d/media__verify_input_disabled.png"
            with open(img_path, "wb") as fh:
                fh.write(base64.b64decode(screenshot_data))
            print(f"Screenshot (disabled) saved to: {img_path}")
            
        # Enable input back
        script_enable = f"toggleInput('{target_input_id}', true)"
        print(f"Sending: {script_enable}")
        send_command(sock, 600, "Runtime.evaluate", {"expression": script_enable, "awaitPromise": True})
        wait_for_response(sock, 600)
        
        time.sleep(2)
        
        # Verify config.json
        print("Reading config.json to verify it is enabled again...")
        with open("/home/cristian/Antigravity/config.json", "r") as f:
            config = json.load(f)
            
        target_input = None
        for inp in config.get("inputs", []):
            if inp.get("id") == target_input_id:
                target_input = inp
                break
                
        if target_input:
            print(f"Config for {target_input_id} after enable:")
            print(json.dumps(target_input, indent=2))
            assert target_input.get("enabled") == True
            print("SUCCESS: config.json correctly updated with enabled = True.")
        else:
            print(f"ERROR: {target_input_id} not found in config.json")
            return
            
    finally:
        chrome_proc.terminate()
        chrome_proc.wait()
        print("Chrome terminated.")

if __name__ == "__main__":
    main()

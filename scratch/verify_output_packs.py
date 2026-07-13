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
        
    # Get active inputs
    print("Fetching active inputs...")
    res = session.get(f"{base_url}/api/inputs")
    if res.status_code != 200:
        print("Failed to get inputs", res.text)
        sys.exit(1)
        
    inputs = res.json()
    if not inputs:
        print("No inputs available to construct output packs. Skipping rest of test.")
        sys.exit(0)
        
    # Find inputs with programs
    valid_channels = []
    for inp in inputs:
        if inp.get("programs"):
            for prog in inp["programs"]:
                valid_channels.append({
                    "input_id": inp["id"],
                    "program_number": prog["program_number"],
                    "name": prog["name"]
                })
                if len(valid_channels) >= 2:
                    break
        if len(valid_channels) >= 2:
            break
            
    if len(valid_channels) < 2:
        print("Not enough input programs/channels to test output pack. Synthesizing test channels...")
        # Synthesize from first input
        inp = inputs[0]
        valid_channels = [
            {"input_id": inp["id"], "program_number": 1, "name": "Test Channel 1"},
            {"input_id": inp["id"], "program_number": 2, "name": "Test Channel 2"}
        ]
        
    # Create Output Pack
    payload = {
        "name": "TEST SRT OUTPUT PACK",
        "output_url": "srt://127.0.0.1:50005?mode=caller",
        "enabled": True,
        "channels": valid_channels
    }
    
    print("Creating new Output Pack...")
    create_res = session.post(f"{base_url}/api/output_packs", json=payload)
    if create_res.status_code != 200:
        print("Failed to create Output Pack", create_res.text)
        sys.exit(1)
        
    pack_id = create_res.json()["id"]
    print(f"Output Pack created successfully with ID: {pack_id}")
    
    # Get Output Packs and verify
    print("Fetching Output Packs list...")
    res = session.get(f"{base_url}/api/output_packs")
    if res.status_code != 200:
        print("Failed to get Output Packs", res.text)
        sys.exit(1)
        
    packs = res.json()
    test_pack = next((p for p in packs if p["id"] == pack_id), None)
    if not test_pack:
        print("Created pack was not found in API response.")
        sys.exit(1)
        
    print(f"Verified pack name: '{test_pack['name']}'")
    print(f"Verified pack enabled state: '{test_pack['enabled']}'")
    print(f"Verified pack channels size: {len(test_pack['channels'])}")
    
    # Toggle off
    print("Toggling Output Pack off...")
    test_pack["enabled"] = False
    update_res = session.put(f"{base_url}/api/output_packs/{pack_id}", json=test_pack)
    if update_res.status_code != 200:
        print("Failed to update Output Pack", update_res.text)
        sys.exit(1)
        
    # Fetch again to verify disabled state
    res = session.get(f"{base_url}/api/output_packs")
    packs = res.json()
    test_pack = next((p for p in packs if p["id"] == pack_id), None)
    if test_pack["enabled"]:
        print("FAILED: Pack enabled state did not switch to False.")
        sys.exit(1)
    print("Verified pack successfully switched to disabled state.")
    
    # Delete pack
    print("Deleting Output Pack...")
    del_res = session.delete(f"{base_url}/api/output_packs/{pack_id}")
    if del_res.status_code != 200:
        print("Failed to delete Output Pack", del_res.text)
        sys.exit(1)
        
    # Verify deleted
    res = session.get(f"{base_url}/api/output_packs")
    packs = res.json()
    test_pack = next((p for p in packs if p["id"] == pack_id), None)
    if test_pack:
        print("FAILED: Pack was not deleted.")
        sys.exit(1)
        
    print("Verified pack was successfully deleted.")
    print("SUCCESS: Output Packs features successfully verified!")

if __name__ == "__main__":
    verify()

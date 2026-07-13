import requests
import json
import sys

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
        
    # Get settings
    print("Fetching current settings...")
    res = session.get(f"{base_url}/api/settings")
    if res.status_code != 200:
        print("Failed to get settings", res.text)
        sys.exit(1)
    
    current_settings = res.json()
    print("Current settings:", current_settings)
    
    # Save settings
    print("Updating presets to p6 and fast...")
    update_res = session.post(f"{base_url}/api/settings", json={
        "nvenc_preset": "p6",
        "cpu_preset": "fast"
    })
    if update_res.status_code != 200 or not update_res.json().get("success"):
        print("Failed to update settings", update_res.text)
        sys.exit(1)
        
    # Verify persistence
    print("Fetching settings again to check persistence...")
    res = session.get(f"{base_url}/api/settings")
    updated = res.json()
    print("Updated settings:", updated)
    if updated.get("nvenc_preset") == "p6" and updated.get("cpu_preset") == "fast":
        print("SUCCESS: Presets successfully updated and persisted!")
    else:
        print("FAILED: Presets did not persist correctly", updated)
        sys.exit(1)
        
    # Restore defaults
    print("Restoring default settings...")
    session.post(f"{base_url}/api/settings", json={
        "nvenc_preset": "p4",
        "cpu_preset": "ultrafast"
    })
    print("VERIFICATION COMPLETED SUCCESSFULLY!")

if __name__ == "__main__":
    verify()

from flask import Flask, request, jsonify
from datetime import datetime
import requests

app = Flask(__name__)

boot_logs = []

@app.route("/pxe-status", methods=["POST"])
def pxe_status():
    data = request.get_json()

    if not data or "client_id" not in data or "status" not in data:
        return jsonify({"error": "Invalid request"}), 400

    log_entry = {
        "client_id": data["client_id"],
        "status": data["status"],
        "timestamp": datetime.utcnow().isoformat()
    }
    
    boot_logs.append(log_entry)

    print(f"[LOG] PXE Client {data['client_id']} -> {data['status']} at {log_entry['timestamp']}")

    return jsonify({"message": "Status received", "entry": log_entry}), 200

@app.route("/pxe-logs", methods=["GET"])
def get_logs():
    return jsonify(boot_logs)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)

import time
import json
import orjson
import cxjson

def run_benchmarks():
    payloads = {
        "1. REST API User Profile": {
            "id": 8392104,
            "username": "johndoe",
            "email": "johndoe@example.com",
            "is_active": True,
            "is_admin": False,
            "profile": {
                "first_name": "John",
                "last_name": "Doe",
                "bio": "Software developer & open source enthusiast.",
                "avatar_url": "https://cdn.example.com/avatars/8392104.png",
                "reputation": 4.88,
                "created_at": "2024-03-15T08:30:00Z"
            },
            "roles": ["member", "contributor", "reviewer"],
            "settings": {
                "theme": "dark",
                "email_notifications": True,
                "sms_notifications": False,
                "two_factor_auth": True,
                "backup_email": None
            }
        },
        "2. E-Commerce Order (with Line Items)": {
            "order_id": "ORD-2026-99812",
            "customer_id": 54210,
            "status": "PROCESSING",
            "currency": "USD",
            "subtotal": 249.95,
            "tax": 18.75,
            "shipping_cost": 9.99,
            "total": 278.69,
            "items": [
                {"sku": "SKU-A101", "name": "Mechanical Keyboard", "quantity": 1, "unit_price": 129.99, "in_stock": True},
                {"sku": "SKU-B202", "name": "Wireless Gaming Mouse", "quantity": 1, "unit_price": 79.99, "in_stock": True},
                {"sku": "SKU-C303", "name": "Desk Mat Large", "quantity": 2, "unit_price": 19.99, "in_stock": True}
            ],
            "shipping_address": {
                "street": "123 Tech Boulevard",
                "suite": "Apt 4B",
                "city": "San Francisco",
                "state": "CA",
                "postal_code": "94107",
                "country": "US"
            },
            "notes": "Please leave package at the front porch if no answer."
        },
        "3. Server Log / Telemetry Event": {
            "timestamp": "2026-10-30T22:50:00.123456Z",
            "level": "INFO",
            "service": "billing-service",
            "environment": "production",
            "host": "ip-10-0-4-122.ec2.internal",
            "trace_id": "8a3b5c7d-9e1f-4a2b-8c3d-0e1f2a3b4c5d",
            "span_id": "f1e2d3c4b5a6",
            "http": {
                "method": "POST",
                "path": "/api/v2/checkout",
                "status_code": 200,
                "latency_ms": 14.82,
                "client_ip": "198.51.100.42"
            },
            "error": None
        },
        "4. Data Table (100 Record Dicts)": [
            {
                "index": i,
                "guid": f"row-{i}-a9b8c7d6",
                "temperature": 20.0 + (i % 15) * 0.75,
                "humidity": 45.0 + (i % 25) * 0.5,
                "active": (i % 2 == 0),
                "device_type": "sensor_v2",
                "location": {"building": f"B{i%5}", "floor": i % 10}
            }
            for i in range(100)
        ],
        "5. Large Data Table (1,000 Record Dicts)": [
            {
                "id": i,
                "name": f"Customer_{i}",
                "balance": 100.0 + (i * 3.75),
                "verified": (i % 3 != 0),
                "tier": "GOLD" if i % 10 == 0 else "STANDARD"
            }
            for i in range(1000)
        ]
    }

    iterations = {
        "1. REST API User Profile": 100_000,
        "2. E-Commerce Order (with Line Items)": 100_000,
        "3. Server Log / Telemetry Event": 100_000,
        "4. Data Table (100 Record Dicts)": 5_000,
        "5. Large Data Table (1,000 Record Dicts)": 500
    }

    header = f"{'Payload Type':<40} | {'std json':<12} | {'cxjson':<12} | {'orjson':<12} | {'vs std':<8} | {'vs orjson'}"
    print(header)
    print("-" * len(header))

    for name, obj in payloads.items():
        n = iterations[name]

        # Warmup
        json.dumps(obj)
        orjson.dumps(obj)
        cxjson.dumps(obj)

        # 1. Standard json
        t0 = time.perf_counter()
        for _ in range(n):
            json.dumps(obj)
        t_std = time.perf_counter() - t0

        # 2. cxjson (bytes)
        t0 = time.perf_counter()
        for _ in range(n):
            cxjson.dumps(obj)
        t_c = time.perf_counter() - t0

        # 3. orjson (bytes)
        t0 = time.perf_counter()
        for _ in range(n):
            orjson.dumps(obj)
        t_or = time.perf_counter() - t0

        ops_std = n / t_std
        ops_c = n / t_c
        ops_or = n / t_or

        speedup_std = f"{t_std / t_c:.1f}x"
        vs_or = f"{ops_c / ops_or:.2f}x ({t_or / t_c:.2f}x)"

        print(f"{name:<40} | {ops_std:>10,.0f} | {ops_c:>10,.0f} | {ops_or:>10,.0f} | {speedup_std:>8} | {vs_or}")

if __name__ == "__main__":
    run_benchmarks()

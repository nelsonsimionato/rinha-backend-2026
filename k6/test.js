import http from 'k6/http';
import { check } from 'k6';
import { SharedArray } from 'k6/data';

// Official data ingestion: Read payloads securely into memory exactly once per test lifecycle
const payloads = new SharedArray('official payloads', function () {
    return JSON.parse(open('../resources/example-payloads.json'));
});

export const options = {
    // Exactly 5,000 iterations mimicking the official evaluation scenario
    iterations: 5000,
    // Distributed across 50 Virtual Users to mimic high-concurrency connection pools
    vus: 50,
    thresholds: {
        // Phase 3 baseline target. Scoring saturates at p99 <= 1ms (+3000) but
        // the brute-force scan over 3M vectors at 0.40 vCPU is memory-bandwidth
        // bound; 500ms keeps us well above the 2000ms -3000 floor while leaving
        // headroom for optimization escalations (3.3 subset, 3.4 AVX2).
        http_req_duration: [{ threshold: 'p(99)<500', abortOnFail: false }],
        http_req_failed: ['rate==0.00'],
    },
};

export default function () {
    const url = 'http://localhost:9999/fraud-score';
    
    // Circular stream logic: Distribute the payload list evenly across all test iterations
    const payloadIndex = __ITER % payloads.length;
    const body = JSON.stringify(payloads[payloadIndex]);

    const params = {
        headers: {
            'Content-Type': 'application/json',
            'Connection': 'keep-alive',
        },
    };

    // Execute Silent Mode POST evaluation
    const res = http.post(url, body, params);

    // Hard assertions mapped to competition rules
    check(res, {
        'status is strictly 200 OK': (r) => r.status === 200,
        'has approved property': (r) => {
            try {
                const b = JSON.parse(r.body);
                return typeof b.approved === 'boolean' && typeof b.fraud_score === 'number';
            } catch (e) {
                return false;
            }
        }
    });
}

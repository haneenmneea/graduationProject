import re
import sys

log_file = sys.argv[1] if len(sys.argv) > 1 else "log.txt"

send_times = {}
latencies = []
total_sent_stable = 0
total_sent_intermittent = 0
total_received = 0

with open(log_file, 'r') as f:
    for line in f:
        parts = line.strip().split(';')
        if len(parts) < 3:
            continue

        try:
            timestamp = float(parts[0])
        except ValueError:
            continue

        node = parts[1]
        message = parts[2]

        # 1. إرسال العقد المستقرة
        if '[INFO: App' in message and 'Sending request' in message:
            send_match = re.search(r'Sending request (\d+)', message)
            if send_match:
                seqno = send_match.group(1)
                send_times[(node, seqno)] = timestamp
                total_sent_stable += 1

        # 2. إرسال العقد المتقطعة (حسب ما يظهر في صورتك)
        elif 'Sending request' in message and 'intermittent node' in message:
            # نلتقط الرقم بعد hello مباشرة
            send_match = re.search(r'hello (\d+)', message)
            if send_match:
                seqno = send_match.group(1)
                send_times[(node, seqno)] = timestamp
                total_sent_intermittent += 1

        # 3. استلام السيرفر (تم تحديثه ليطابق الصورة تماماً)
        if "Received request" in message and node == 'm3-1':
            # لاحظ المسافة الاختيارية \s* قبل كلمة hello للتعامل مع الفراغات
            recv_match = re.search(r"Received request '\s*hello (\d+).*?' from (fd00::[a-fA-F0-9:]+)", message)
            if recv_match:
                seqno = recv_match.group(1)
                total_received += 1
                
                # مطابقة الوقت لحساب Latency
                for (n, s), t in list(send_times.items()):
                    if s == seqno:
                        latency = (timestamp - t) * 1000
                        if 0 < latency < 15000: # رفعنا الحد قليلاً للـ Intermittent
                            latencies.append(latency)
                            del send_times[(n, s)]
                            break

# تعريف vals دائماً لتجنب خطأ الـ Traceback
vals = latencies
total_sent_all = total_sent_stable + total_sent_intermittent
pdr_all = (total_received / total_sent_all * 100) if total_sent_all > 0 else 0

print(f"{'='*50}")
print(f"Log File:                    {log_file}")
print(f"{'='*50}")
print(f"Sent (stable):               {total_sent_stable}")
print(f"Sent (intermittent):         {total_sent_intermittent}")
print(f"Total Sent:                  {total_sent_all}")
print(f"Total Received by Server:    {total_received}")
print(f"PDR (Overall):               {pdr_all:.2f}%")
print(f"{'='*50}")

if vals:
    print(f"Valid Latency Samples:       {len(vals)}")
    print(f"Avg Latency:                 {sum(vals)/len(vals):.2f} ms")
else:
    print("No valid latency data found! Check if Sequence Numbers match.")

#!/usr/bin/env python3
"""Write leads.csv: 300 synthetic sales leads -- industry, company size, web
visits, downloads, how the lead came in -- of which 240 are decided (Outcome
'won' or 'lost') and 60 still open (Outcome empty). Load it into a SharePoint
list with lead_scoring.sql's first step; the model then scores the open ones.

    python3 make_sample_data.py

Made up: a lead is likelier won with more visits and downloads, from a
referral, and in software or manufacturing.
"""
import csv
import math
import random
from pathlib import Path

random.seed(53)
OUT = Path(__file__).resolve().parent / "leads.csv"
INDUSTRIES = {"Software": 0.8, "Manufacturing": 0.5, "Retail": -0.3, "Logistics": 0.0, "Public sector": -0.6}
SOURCES = {"Referral": 1.0, "Webinar": 0.3, "Website": 0.0, "Trade fair": 0.2, "Cold call": -0.8}

with OUT.open("w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Title", "Industry", "Employees", "Visits", "Downloads", "Source", "Outcome"])
    for i in range(1, 301):
        industry = random.choice(list(INDUSTRIES))
        source = random.choice(list(SOURCES))
        employees = int(math.exp(random.uniform(math.log(10), math.log(5000))))
        visits = random.randint(0, 30)
        downloads = random.randint(0, 6)
        logit = -2.0 + 0.12 * visits + 0.35 * downloads + INDUSTRIES[industry] + SOURCES[source]
        won = random.random() < 1 / (1 + math.exp(-logit))
        outcome = "" if i > 240 else ("won" if won else "lost")
        w.writerow([f"Lead {i:03d}", industry, employees, visits, downloads, source, outcome])
print(OUT)

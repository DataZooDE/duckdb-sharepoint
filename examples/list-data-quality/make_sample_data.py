#!/usr/bin/env python3
"""Write customers.csv: 40 synthetic customers with the problems a real
customer list has -- mistyped emails and phone numbers, wrong VAT check
digits, a broken IBAN, a duplicate, missing fields, and personal data in the
free-text notes. Every name, address and number is made up (example.com,
documentation IBANs); load it into a SharePoint list with
data_quality.sql's first step.

    python3 make_sample_data.py
"""
import csv
import random
from pathlib import Path

random.seed(7)
OUT = Path(__file__).resolve().parent / "customers.csv"

FIRST = ["Anna", "Ben", "Clara", "David", "Eva", "Felix", "Greta", "Hannah", "Jonas", "Lena", "Max", "Nina",
         "Oskar", "Paula", "Rosa", "Theo", "Uwe", "Vera", "Wim", "Zoe"]
LAST = ["Becker", "Fischer", "Hoffmann", "Klein", "Koch", "Lange", "Meyer", "Neumann", "Richter", "Schmidt"]
COUNTRIES = [("DE", "+49 30 901820", "DE136695976", "DE89370400440532013000"),
             ("FR", "+33 1 2345 67", "FR40303265045", "FR1420041010050500013M02606"),
             ("NL", "+31 20 55239", "NL004495445B01", "NL91ABNA0417164300")]

rows = []
for i in range(1, 41):
    first, last = FIRST[i % len(FIRST)], LAST[i % len(LAST)]
    country, phone, vat, iban = COUNTRIES[i % 3]
    email = f"{first}.{last}{i}@example.com".lower()
    phone = f"{phone}{i:02d}"
    notes = random.choice(["Prefers invoices by post.", "Key account.", "", "Asked for a call back in Q1."])
    if i % 9 == 0:
        email = email.replace("@", "")                      # not an email address
    if i % 11 == 0:
        phone = "12345"                                     # not a dialable number
    if i % 13 == 0:
        vat = vat[:-1] + str((int(vat[-1]) + 1) % 10) if vat[-1].isdigit() else vat[:-2] + "99"  # bad check digit
    if i == 17:
        iban = iban[:-3] + str((int(iban[-3]) + 1) % 10) + iban[-2:]  # one digit off: the check digits fail
    if i % 10 == 0:
        notes = f"Called {first} on {phone}, private mail {first.lower()}@home.example.com"  # PII in free text
    if i == 23:
        notes = "Pays by card 4111 1111 1111 1111"
    if i in (5, 29):
        vat = ""                                            # missing
    rows.append({"Title": f"{first} {last}", "Email": email, "Phone": phone, "Country": country,
                 "VAT": vat, "IBAN": iban, "Notes": notes})
rows.append(dict(rows[3]))                                  # a duplicate customer

with OUT.open("w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0]))
    w.writeheader()
    w.writerows(rows)
print(OUT)

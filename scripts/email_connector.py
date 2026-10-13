#!/usr/bin/env python3
"""
Amuse Real Email & Contacts Connector (On-Device Thunderbird + Evolution + Hosted IMAP/SMTP)
"""
import datetime
import email
from email.header import decode_header
from email.message import EmailMessage
import glob
import imaplib
import json
import os
import re
import smtplib
import sqlite3
import subprocess
import sys

AMUSE_DIR = os.path.expanduser("~/.amuse")
ACCOUNTS_FILE = os.path.join(AMUSE_DIR, "email_accounts.json")


def find_thunderbird_dbs():
    patterns = [
        os.path.expanduser("~/snap/thunderbird/common/.thunderbird/*/global-messages-db.sqlite"),
        os.path.expanduser("~/.thunderbird/*/global-messages-db.sqlite"),
    ]
    found = []
    for pat in patterns:
        for p in glob.glob(pat):
            try:
                if os.path.getsize(p) > 2000000:  # non-empty DB (>2MB)
                    found.append(p)
            except OSError:
                pass
    if not found:
        for pat in patterns:
            found.extend(glob.glob(pat))
    # Sort largest first
    found.sort(key=lambda x: os.path.getsize(x) if os.path.exists(x) else 0, reverse=True)
    return found


def load_hosted_accounts():
    if not os.path.exists(ACCOUNTS_FILE):
        return []
    try:
        with open(ACCOUNTS_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
            return data if isinstance(data, list) else []
    except Exception:
        return []


def save_hosted_account(account_info):
    os.makedirs(AMUSE_DIR, exist_ok=True)
    accounts = load_hosted_accounts()
    email_addr = account_info.get("email", "").strip()
    accounts = [a for a in accounts if a.get("email") != email_addr]
    accounts.append(account_info)
    with open(ACCOUNTS_FILE, "w", encoding="utf-8") as f:
        json.dump(accounts, f, indent=2)
    try:
        os.chmod(ACCOUNTS_FILE, 0o600)
    except OSError:
        pass


def get_thunderbird_accounts(db_path):
    accounts = []
    try:
        conn = sqlite3.connect(f"file:{db_path}?immutable=1", uri=True)
        cur = conn.cursor()
        rows = cur.execute("SELECT name, folderURI FROM folderLocations").fetchall()
        seen = set()
        for name, uri in rows:
            if not uri:
                continue
            m = re.search(r"imap://([^@/]+)@([^/]+)", uri)
            if m:
                user = m.group(1).replace("%40", "@")
                host = m.group(2)
                key = f"{user} ({host})"
                if key not in seen:
                    seen.add(key)
                    accounts.append({"account": user, "server": host, "source": "on_device_thunderbird"})
        conn.close()
    except Exception:
        pass
    return accounts


def search_on_device_emails(query="", sender="", limit=12):
    dbs = find_thunderbird_dbs()
    results = []
    accounts = []
    total_indexed = 0

    for db_path in dbs:
        accounts.extend(get_thunderbird_accounts(db_path))
        try:
            conn = sqlite3.connect(f"file:{db_path}?immutable=1", uri=True)
            cur = conn.cursor()
            cnt_row = cur.execute("SELECT COUNT(*) FROM messages WHERE deleted = 0").fetchone()
            if cnt_row:
                total_indexed += cnt_row[0]

            sql = """
                SELECT m.id, m.date, t.c1subject, t.c3author, t.c4recipients,
                       substr(t.c0body, 1, 500), f.name, f.folderURI
                FROM messages m
                JOIN messagesText_content t ON m.id = t.docid
                LEFT JOIN folderLocations f ON m.folderID = f.id
                WHERE m.deleted = 0
            """
            params = []
            if query:
                sql += " AND (t.c1subject LIKE ? COLLATE NOCASE OR t.c0body LIKE ? COLLATE NOCASE OR t.c3author LIKE ? COLLATE NOCASE)"
                q = f"%{query}%"
                params.extend([q, q, q])
            if sender:
                sql += " AND (t.c3author LIKE ? COLLATE NOCASE OR t.c4recipients LIKE ? COLLATE NOCASE)"
                s = f"%{sender}%"
                params.extend([s, s])

            sql += f" ORDER BY m.date DESC LIMIT {int(limit)}"
            rows = cur.execute(sql, params).fetchall()
            for r in rows:
                msg_id, ts_usec, subj, auth, recip, body, folder_name, folder_uri = r
                dt_str = ""
                if ts_usec:
                    try:
                        dt_str = datetime.datetime.fromtimestamp(ts_usec / 1000000.0).strftime("%Y-%m-%d %H:%M:%S")
                    except Exception:
                        dt_str = str(ts_usec)
                clean_auth = (auth or "").replace(" undefined", "").strip()
                clean_recip = (recip or "").replace(" undefined", "").strip()
                clean_body = re.sub(r"\s+", " ", (body or "")).strip()
                results.append({
                    "id": f"tb-{msg_id}",
                    "source": "on_device_thunderbird",
                    "folder": folder_name or "Inbox",
                    "date": dt_str,
                    "sender": clean_auth,
                    "recipient": clean_recip,
                    "subject": subj or "(No Subject)",
                    "snippet": clean_body[:280],
                })
            conn.close()
            if results:
                break
        except Exception as e:
            results.append({"error": str(e), "db": db_path})

    return {
        "on_device_client": "Thunderbird",
        "total_on_device_messages": total_indexed,
        "connected_on_device_accounts": accounts,
        "messages": results,
    }


def decode_mime_str(val):
    if not val:
        return ""
    parts = decode_header(val)
    out = []
    for content, enc in parts:
        if isinstance(content, bytes):
            out.append(content.decode(enc or "utf-8", errors="replace"))
        else:
            out.append(str(content))
    return "".join(out)


def fetch_hosted_imap_emails(query="", limit=10):
    accounts = load_hosted_accounts()
    hosted_messages = []
    account_statuses = []

    for acc in accounts:
        email_addr = acc.get("email", "")
        password = acc.get("password", "")
        imap_host = acc.get("imap_host", "imap.gmail.com")
        imap_port = int(acc.get("imap_port", 993))
        if not email_addr or not password:
            continue
        try:
            M = imaplib.IMAP4_SSL(imap_host, imap_port, timeout=8)
            M.login(email_addr, password)
            M.select("INBOX", readonly=True)
            search_crit = "ALL"
            if query:
                safe_q = query.replace('"', "")
                search_crit = f'(OR SUBJECT "{safe_q}" FROM "{safe_q}")'
            typ, data = M.search(None, search_crit)
            if typ == "OK" and data and data[0]:
                ids = data[0].split()[-limit:]
                ids.reverse()
                for num in ids:
                    typ, msg_data = M.fetch(num, "(RFC822.HEADER BODY.PEEK[TEXT]<0.400>)")
                    if typ == "OK" and msg_data and msg_data[0]:
                        raw_headers = msg_data[0][1]
                        msg = email.message_from_bytes(raw_headers)
                        subj = decode_mime_str(msg.get("Subject", ""))
                        from_ = decode_mime_str(msg.get("From", ""))
                        to_ = decode_mime_str(msg.get("To", ""))
                        date_ = decode_mime_str(msg.get("Date", ""))
                        snippet = ""
                        if len(msg_data) > 1 and isinstance(msg_data[1], tuple):
                            snippet = msg_data[1][1].decode("utf-8", errors="replace")[:240]
                        hosted_messages.append({
                            "id": f"imap-{email_addr}-{num.decode()}",
                            "source": f"hosted_imap:{email_addr}",
                            "folder": "INBOX",
                            "date": date_,
                            "sender": from_,
                            "recipient": to_,
                            "subject": subj,
                            "snippet": re.sub(r"\s+", " ", snippet).strip(),
                        })
            M.logout()
            account_statuses.append({"email": email_addr, "imap_host": imap_host, "status": "connected"})
        except Exception as e:
            account_statuses.append({"email": email_addr, "imap_host": imap_host, "status": f"error: {e}"})

    return {
        "hosted_accounts": account_statuses,
        "hosted_messages": hosted_messages,
    }


def connect_hosted_account(email_addr, password, provider="gmail", imap_host="", smtp_host=""):
    p = (provider or "gmail").lower()
    if not imap_host:
        if "outlook" in p or "office365" in p or "uh.edu" in email_addr:
            imap_host = "outlook.office365.com"
        else:
            imap_host = "imap.gmail.com"
    if not smtp_host:
        if "outlook" in p or "office365" in p or "uh.edu" in email_addr:
            smtp_host = "smtp.office365.com"
        else:
            smtp_host = "smtp.gmail.com"

    acc = {
        "email": email_addr,
        "password": password,
        "provider": provider,
        "imap_host": imap_host,
        "imap_port": 993,
        "smtp_host": smtp_host,
        "smtp_port": 465 if "gmail" in smtp_host else 587,
    }
    save_hosted_account(acc)

    # Test IMAP connection if password provided
    verified = False
    verify_msg = "Saved credentials to ~/.amuse/email_accounts.json"
    try:
        M = imaplib.IMAP4_SSL(imap_host, 993, timeout=8)
        M.login(email_addr, password)
        M.select("INBOX", readonly=True)
        verified = True
        verify_msg = f"Successfully authenticated with {imap_host} via IMAP4_SSL"
        M.logout()
    except Exception as e:
        verify_msg = f"Saved credentials (live IMAP check note: {e})"

    return {
        "success": True,
        "email": email_addr,
        "imap_host": imap_host,
        "smtp_host": smtp_host,
        "verified": verified,
        "message": verify_msg,
    }


def send_or_compose_email(to_addr, subject, body, attachment_path="", use_on_device_client=True, send_immediately=False):
    accounts = load_hosted_accounts()
    sent_via_smtp = False
    smtp_status = ""

    # 1. If hosted SMTP account is configured and send_immediately is requested, send directly via SMTP
    if accounts and send_immediately and to_addr:
        acc = accounts[0]
        try:
            msg = EmailMessage()
            msg["From"] = acc["email"]
            msg["To"] = to_addr
            msg["Subject"] = subject
            msg.set_content(body)
            if attachment_path and os.path.exists(attachment_path):
                with open(attachment_path, "rb") as f:
                    file_data = f.read()
                msg.add_attachment(
                    file_data,
                    maintype="application",
                    subtype="octet-stream",
                    filename=os.path.basename(attachment_path),
                )
            smtp_host = acc.get("smtp_host", "smtp.gmail.com")
            smtp_port = int(acc.get("smtp_port", 465))
            if smtp_port == 465:
                with smtplib.SMTP_SSL(smtp_host, smtp_port, timeout=10) as s:
                    s.login(acc["email"], acc["password"])
                    s.send_message(msg)
            else:
                with smtplib.SMTP(smtp_host, smtp_port, timeout=10) as s:
                    s.starttls()
                    s.login(acc["email"], acc["password"])
                    s.send_message(msg)
            sent_via_smtp = True
            smtp_status = f"Sent directly via SMTP ({acc['email']})"
        except Exception as e:
            smtp_status = f"SMTP send failed ({e}); falling back to desktop client"

    # 2. Launch on-device Thunderbird compose window if available
    launched_thunderbird = False
    if use_on_device_client and not sent_via_smtp:
        tb_bin = "/snap/bin/thunderbird" if os.path.exists("/snap/bin/thunderbird") else "thunderbird"
        safe_to = to_addr.replace("'", "")
        safe_subj = subject.replace("'", "")
        safe_body = body.replace("'", "")
        compose_arg = f"to='{safe_to}',subject='{safe_subj}',body='{safe_body}'"
        if attachment_path and os.path.exists(attachment_path):
            compose_arg += f",attachment='file://{os.path.abspath(attachment_path)}'"
        try:
            subprocess.Popen(
                [tb_bin, "-compose", compose_arg],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            launched_thunderbird = True
        except Exception:
            pass

    return {
        "success": True,
        "to": to_addr,
        "subject": subject,
        "sent_via_hosted_smtp": sent_via_smtp,
        "smtp_status": smtp_status,
        "launched_on_device_thunderbird_compose": launched_thunderbird,
    }


def search_on_device_contacts(query="", limit=15):
    dbs = find_thunderbird_dbs()
    contacts = []
    for db_path in dbs:
        try:
            conn = sqlite3.connect(f"file:{db_path}?immutable=1", uri=True)
            cur = conn.cursor()
            sql = "SELECT name, popularity FROM contacts WHERE name != ''"
            params = []
            if query:
                sql += " AND name LIKE ? COLLATE NOCASE"
                params.append(f"%{query}%")
            sql += f" ORDER BY popularity DESC LIMIT {int(limit)}"
            for name, pop in cur.execute(sql, params).fetchall():
                contacts.append({
                    "name": name,
                    "popularity": pop,
                    "source": "on_device_thunderbird",
                })
            conn.close()
            if contacts:
                break
        except Exception:
            pass
    return {"contacts": contacts}


def main():
    if len(sys.argv) < 2:
        args = {"action": "search"}
        action = "search"
    elif sys.argv[1].strip().startswith("{"):
        try:
            args = json.loads(sys.argv[1])
        except Exception:
            args = {}
        action = args.get("action", "search")
    else:
        action = sys.argv[1]
        args = {}
        if len(sys.argv) >= 3:
            try:
                args = json.loads(sys.argv[2])
            except Exception:
                args = {}

    if action in ("search", "check", "list", "read_latest", "list_accounts"):
        query = args.get("query", "") or args.get("subject", "")
        sender = args.get("sender", "") or args.get("to", "")
        limit = int(args.get("limit", 10))
        if action == "read_latest" and limit > 5:
            limit = 5
        on_device = search_on_device_emails(query=query, sender=sender, limit=limit)
        hosted = fetch_hosted_imap_emails(query=query, limit=limit)
        out = {
            "success": True,
            "action": action,
            "on_device_client": on_device["on_device_client"],
            "total_on_device_messages": on_device["total_on_device_messages"],
            "connected_on_device_accounts": on_device["connected_on_device_accounts"],
            "hosted_imap_accounts": hosted["hosted_accounts"],
            "hosted_messages": hosted["hosted_messages"],
            "on_device_messages": on_device["messages"],
            "note": (
                "These are REAL emails from the user's on-device Thunderbird database and/or connected hosted IMAP accounts. "
                "To connect an additional hosted IMAP/SMTP account (Gmail App Password, Office365, etc.), call connect_email_account."
            ),
        }
        print(json.dumps(out))
    elif action == "connect_account":
        res = connect_hosted_account(
            email_addr=args.get("email", ""),
            password=args.get("password", ""),
            provider=args.get("provider", "gmail"),
            imap_host=args.get("imap_host", ""),
            smtp_host=args.get("smtp_host", ""),
        )
        print(json.dumps(res))
    elif action in ("compose", "draft", "send", "open_composer"):
        res = send_or_compose_email(
            to_addr=args.get("to", ""),
            subject=args.get("subject", ""),
            body=args.get("body", ""),
            attachment_path=args.get("attachment_path", ""),
            use_on_device_client=args.get("use_on_device_client", True),
            send_immediately=(action == "send"),
        )
        print(json.dumps(res))
    elif action == "contacts":
        res = search_on_device_contacts(query=args.get("query", ""), limit=int(args.get("limit", 15)))
        print(json.dumps(res))
    else:
        print(json.dumps({"error": f"Unknown action: {action}"}))


if __name__ == "__main__":
    main()

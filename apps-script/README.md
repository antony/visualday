# Calendar feed setup

The device reads your calendar through a small Google Apps Script that you
deploy from your own account. Takes about five minutes.

1. Go to <https://script.google.com> and click **New project**. Name it
   `VisualDay`.
2. Replace everything in `Code.gs` with the contents of [`Code.gs`](Code.gs) and
   save.
3. For bin days: in the left sidebar, click **+** next to **Services**, pick
   **Google Tasks API** and click **Add**. (Skip this and everything else still
   works, just without bins.)
4. In the toolbar, pick the `setup` function and click **Run**. Google asks you
   to authorise it: choose your account, then **Advanced > Go to VisualDay
   (unsafe)** (it's "unsafe" only because it's your own unverified script), then
   **Allow**. The execution log shows a line like `Token: 3f9c...`, and any bin
   tasks due today or tomorrow.
5. Click **Deploy > New deployment**, choose the type **Web app**, and set:
   - Execute as: **Me**
   - Who has access: **Anyone**

   Click **Deploy** and copy the **Web app URL** (ends in `/exec`).
6. Your feed URL is the web app URL plus `?token=` and your token:

   ```
   https://script.google.com/macros/s/AKfy.../exec?token=3f9c...
   ```

   Open it in a browser: you should see `{"events":[...]}`. Without the token
   you get `{"error":"bad token"}`, which is how it should be.
7. Give that URL to the device: hold the top button while switching it on, join
   the **VisualDay** hotspot, choose **Configure WiFi** and paste it into
   **Calendar feed URL**.

**Bin days** come from Google Tasks titled exactly `Green Bin`, `Brown Bin` or
`Black Bin` (any capitals), in any task list. A bin shows on the day the task is
due, until you tick it off. Repeating tasks work.

To include more than your main calendar, list their IDs in `CALENDAR_IDS` at the
top of `Code.gs` (find an ID under the calendar's **Settings > Integrate
calendar**). After editing the script, redeploy via **Deploy > Manage
deployments**, click the pencil, set **Version: New version** and **Deploy**, so
the URL stays the same. (Updating to bin support? Add the Tasks service and run
`setup` again first, to grant the new permission.)

Anyone with the full URL (including the token) can read your events for any
day, so treat it like a password. To revoke it, delete the `TOKEN` script
property (**Project Settings > Script properties**) and run `setup` again.

"use client";
// SPDX-License-Identifier: Apache-2.0
//
// Cases: every case of both stores on the same grid, so case n of one store sits
// directly above case n of the other. Status is an icon, a label and the number.
import type { CaseRun, CaseType, Mode, StoreModel } from "@coldkeep/shared";
import { Card, PageHeader, StoreName } from "@/components/kit";
import { Waiting } from "@/components/Shell";
import { StatusBadge } from "@/components/Status";
import { CASE_TYPE_LABEL, caseStatus } from "@/lib/derive";
import { useRelay } from "@/lib/relay";

const ORDER: Mode[] = ["legacy", "protected"];

const TYPE: Record<CaseType, string> = { chilled: "Chilled", frozen: "Frozen", coldroom: "Cold room" };

function CaseTile({ mode, c }: { mode: Mode; c: CaseRun }) {
  const st = caseStatus(c);
  const l = c.latest;
  const prod = l ? l.prod_true ?? l.prod : null;
  return (
    <article
      className="flex min-w-0 flex-col gap-3 bg-card p-3"
      data-testid={`case-${mode}-${c.info.id}`}
      data-status={st}
      title={`${CASE_TYPE_LABEL[c.info.type]}, limit ${c.info.limit} °C`}
    >
      <div className="flex items-baseline justify-between gap-2">
        <span className="mono-label text-foreground">{String(c.info.id).padStart(2, "0")}</span>
        <span className="mono-label truncate">{TYPE[c.info.type]}</span>
      </div>
      <div className="num font-mono text-xl tracking-tight">
        {prod === null ? "—" : prod.toFixed(1)}
        <span className="ml-1 text-xs text-faint">°C</span>
      </div>
      <div className="flex items-center justify-between gap-2">
        <StatusBadge status={st} />
        {!!l?.hold && <span className="mono-label text-good-ink">G6 hold</span>}
      </div>
    </article>
  );
}

function StoreRow({ s }: { s: StoreModel }) {
  const cases = s.cases.filter(Boolean);
  const over = cases.filter((c) => caseStatus(c) === "over").length;
  return (
    <Card
      title={<StoreName mode={s.mode} />}
      actions={<span className="mono-label">{over} / {cases.length} over limit</span>}
      bodyClassName="p-0"
    >
      <div className="grid grid-cols-2 gap-px bg-border sm:grid-cols-4 xl:grid-cols-8">
        {cases.map((c) => <CaseTile key={c.info.id} mode={s.mode} c={c} />)}
      </div>
    </Card>
  );
}

export default function CasesPage() {
  const r = useRelay();
  const stores = ORDER.map((m) => r.stores[m]).filter((s): s is StoreModel => !!s && s.cases.some((c) => c?.latest));
  if (!stores.length) return <Waiting what="telemetry" />;
  return (
    <div className="flex flex-col gap-6">
      <PageHeader title="Cases" />
      {stores.map((s) => <StoreRow key={s.mode} s={s} />)}
    </div>
  );
}

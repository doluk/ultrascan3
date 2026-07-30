#set document(title: [{{REPORT_TITLE}}])
#set page(
  paper: "us-letter",
  margin: (x: 0.6in, y: 0.6in),
  numbering: "1 / 1",
)
#set heading(numbering: "1.")
#set text(size: 9pt)
#set par(justify: true, leading: 0.65em)

#align(center)[#text(size: 13pt, weight: "bold")[{{REPORT_TITLE}}]]
#v(0.8em)

{{REPORT_BODY}}

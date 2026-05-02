# 1. 添加所有修改
echo "🔍 正在添加所有修改文件..."
git add .

# 2. 提交代码
echo "📝 正在提交代码..."
git commit -m "代码规范"

# 3. 推送到远端test分支
echo "🚀 正在推送到远端 test 分支..."
git push origin test

echo ""
echo "✅ 推送完成！代码已同步到 GitHub test 分支"
echo ""
